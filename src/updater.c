// updater.c -- App Museum II update check. See updater.h for the design rules.
//
// Protocol (plain HTTP, no TLS on this device):
//   GET http://appcatalog.webosarchive.org/WebService/getLatestVersionInfo.php
//       ?app=<Name>/<version>
//   -> {"version":"1.2.0","versionNote":"...","downloadURI":"..."}
//   -> {"error":"No matching app found for ..."}    when unlisted
//
// Verified from inside the PDK jail: DNS resolves and port 80 connects even
// though the jail has no /etc/resolv.conf -- glibc falls back to querying
// 127.0.0.1, where this device runs a resolver. Do not assume that; it was
// measured, because gethostbyname() failing in the jail is what used to crash
// this game's UDP_Init.
#include "quakedef.h"
#include "updater.h"

#ifdef __webos__

#include <pthread.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define UPDATE_HOST   "appcatalog.webosarchive.org"
#define UPDATE_PORT   80
#define UPDATE_PATH   "/WebService/getLatestVersionInfo.php?app="

#define HTTP_BUF_SIZE 4096
#define VERSION_SIZE  32
#define NET_TIMEOUT   10        /* seconds; a wedged server must not linger */
#define ANNOUNCE_DELAY 6.0      /* engine seconds to wait before speaking up */

// Written by the worker, read by the main thread. The worker fills the string
// FIRST and raises the flag LAST, so the main thread can never observe a flag
// without its data.
static volatile int  g_update_ready;
static volatile int  g_announced;
static volatile int  g_started;
static char          g_new_version[VERSION_SIZE];

static pthread_t     g_thread;
static int           g_thread_live;

/*
=================
http_get

Minimal HTTP/1.0 GET. "Connection: close" so there is no chunked encoding to
decode and the body simply runs to EOF. Returns body length, or -1.
=================
*/
static int http_get (const char *host, int port, const char *path,
                     char *out, int out_size)
{
    struct hostent    *he;
    struct sockaddr_in addr;
    struct timeval     tv;
    char               request[512];
    char               raw[HTTP_BUF_SIZE];
    char              *body;
    int                sock, sent, total, n, raw_len;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
        return -1;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    tv.tv_sec = NET_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: QuakeHD-webOS\r\n"
             "Connection: close\r\n"
             "\r\n", path, host);

    total = (int)strlen(request);
    for (sent = 0; sent < total; ) {
        n = send(sock, request + sent, total - sent, 0);
        if (n <= 0) { close(sock); return -1; }
        sent += n;
    }

    for (raw_len = 0; raw_len < (int)sizeof(raw) - 1; ) {
        n = recv(sock, raw + raw_len, sizeof(raw) - 1 - raw_len, 0);
        if (n <= 0) break;
        raw_len += n;
    }
    raw[raw_len] = '\0';
    close(sock);

    body = strstr(raw, "\r\n\r\n");
    if (!body)
        return -1;
    body += 4;

    n = raw_len - (int)(body - raw);
    if (n <= 0)
        return -1;
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, body, n);
    out[n] = '\0';
    return n;
}

/*
=================
json_get_string

Pulls "key":"value" out of a flat JSON object. Deliberately tiny -- the two
replies this talks to are flat, and a real parser is not worth the dependency.
=================
*/
static int json_get_string (const char *json, const char *key,
                            char *out, int out_size)
{
    char        pattern[64];
    const char *p;
    int         len;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    for (len = 0; *p && *p != '"' && len < out_size - 1; p++) {
        if (*p == '\\' && p[1]) {          /* only escapes these replies use */
            p++;
            switch (*p) {
                case 'n': out[len++] = '\n'; break;
                case 't': out[len++] = '\t'; break;
                case 'r': break;
                default:  out[len++] = *p;   break;
            }
        } else {
            out[len++] = *p;
        }
    }
    out[len] = '\0';
    return 1;
}

/*
=================
version_is_newer

major.minor.build. A version that will not parse is treated as "not newer", so
a malformed reply can never nag the player.
=================
*/
static int version_is_newer (const char *local, const char *remote)
{
    int lma = 0, lmi = 0, lbu = 0;
    int rma = 0, rmi = 0, rbu = 0;

    if (sscanf(local,  "%d.%d.%d", &lma, &lmi, &lbu) != 3) return 0;
    if (sscanf(remote, "%d.%d.%d", &rma, &rmi, &rbu) != 3) return 0;

    if (rma != lma) return rma > lma;
    if (rmi != lmi) return rmi > lmi;
    return rbu > lbu;
}

// URL-encode just enough for an app name: spaces become %20, alphanumerics and
// a few safe characters pass through, everything else is escaped.
static void url_encode (const char *in, char *out, int out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;

    for (; *in && o < out_size - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xf];
            out[o++] = hex[c & 0xf];
        }
    }
    out[o] = '\0';
}

/*
=================
Update_Thread

Touches no engine state. Fills g_new_version, then raises g_update_ready.
=================
*/
static void *Update_Thread (void *arg)
{
    char body[HTTP_BUF_SIZE];
    char query[256], path[320], version[VERSION_SIZE];

    (void)arg;

    url_encode(UPDATER_APP_NAME "/" QUAKEHD_VERSION, query, sizeof(query));
    snprintf(path, sizeof(path), "%s%s", UPDATE_PATH, query);

    if (http_get(UPDATE_HOST, UPDATE_PORT, path, body, sizeof(body)) <= 0)
        return NULL;                                   /* offline: say nothing */

    // An unlisted app answers {"error":"No matching app found for ..."} and has
    // no "version" field at all, so this covers it without a special case.
    if (!json_get_string(body, "version", version, sizeof(version)))
        return NULL;
    if (!version_is_newer(QUAKEHD_VERSION, version))
        return NULL;

    strncpy(g_new_version, version, VERSION_SIZE - 1);
    g_new_version[VERSION_SIZE - 1] = '\0';

    // Publish the string before the flag.
    __sync_synchronize();
    g_update_ready = 1;
    return NULL;
}

void Updater_Init (void)
{
    if (g_started || COM_CheckParm("-noupdatecheck"))
        return;
    g_started = 1;

    if (pthread_create(&g_thread, NULL, Update_Thread, NULL) == 0)
        g_thread_live = 1;
}

void Updater_Poll (void)
{
    if (!g_update_ready || g_announced)
        return;

    // Hold the notice back until the engine has settled. Quake's notify area is
    // four lines that live for con_notifytime (3s), and the tail of Host_Init
    // plus the startup demo print enough to scroll it away before the player
    // can read it. By this point the console is quiet and the line stands alone.
    if (realtime < ANNOUNCE_DELAY)
        return;

    g_announced = 1;

    // Con_Printf also lands in the on-screen notify area for a few seconds,
    // which is the whole user-facing feature.
    Con_Printf("\nAn update is available in the App Catalog (%s %s)\n",
               UPDATER_APP_NAME, g_new_version);
}

void Updater_Shutdown (void)
{
    if (g_thread_live) {
        pthread_join(g_thread, NULL);
        g_thread_live = 0;
    }
}

#endif // __webos__

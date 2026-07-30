// netprobe.c -- can a JAILED PDK app resolve a hostname and make an HTTP
// request? The jail has no /etc/resolv.conf, and gethostbyname() is known to
// fail there for the local hostname (it crashed stock Quake's UDP_Init), so
// this has to be proven before building an update checker on top of it.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST "appcatalog.webosarchive.org"
#define PATH "/WebService/getLatestVersionInfo.php?app=EMU7800/1.0.0"

int main(void)
{
    struct hostent *he;
    struct sockaddr_in addr;
    struct timeval tv;
    int sock, n;
    char req[512], buf[2048];

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== netprobe (uid=%d) ==\n", (int)getuid());

    he = gethostbyname(HOST);
    if (!he) {
        printf("FAIL gethostbyname(%s): h_errno=%d\n", HOST, h_errno);
        return 1;
    }
    printf("OK   resolved %s -> %s\n", HOST,
           inet_ntoa(*(struct in_addr *)he->h_addr_list[0]));

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { printf("FAIL socket: %d\n", errno); return 1; }

    tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(80);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("FAIL connect: errno=%d\n", errno);
        close(sock);
        return 1;
    }
    printf("OK   connected\n");

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             PATH, HOST);
    if (send(sock, req, strlen(req), 0) <= 0) {
        printf("FAIL send: errno=%d\n", errno);
        close(sock);
        return 1;
    }

    n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (n <= 0) { printf("FAIL recv: n=%d errno=%d\n", n, errno); return 1; }
    buf[n] = 0;
    printf("OK   got %d bytes\n", n);
    {
        char *body = strstr(buf, "\r\n\r\n");
        printf("BODY: %.200s\n", body ? body + 4 : "(no body)");
    }
    printf("== netprobe DONE ==\n");
    return 0;
}

/* win32.h
 *
 */

#ifndef WIN32_H
#define WIN32_H

#undef  _WIN32_WINNT
#define _WIN32_WINNT    0x0501        /* Needed to resolve getaddrinfo et al. */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <io.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <process.h>

#define EWOULDBLOCK        EAGAIN
#define EAFNOSUPPORT       47
#define EADDRINUSE         WSAEADDRINUSE
#define EAI_SYSTEM         -11

#ifndef SIGHUP
#define SIGHUP -1
#endif

typedef char *caddr_t;

#define O_BLOCK 0
#define O_NONBLOCK 1
#define F_GETFL 3
#define F_SETFL 4

#define IOV_MAX 1024

struct msghdr {
    void         *msg_name;         /* Socket name            */
    int          msg_namelen;       /* Length of name        */
    struct iovec *msg_iov;          /* Data blocks            */
    int          msg_iovlen;        /* Number of blocks        */
    void         *msg_accrights;    /* Per protocol magic (eg BSD file descriptor passing) */
    int          msg_accrightslen;  /* Length of rights list */
};

#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif

int fcntl(SOCKET s, int cmd, int val);
int inet_aton(register const char *cp, struct in_addr *addr);

#define close(s) closesocket(s)

static inline void mapErr(int error) {
    switch(error) {
        default:
            break;
        case WSAEPFNOSUPPORT:
            errno = EAFNOSUPPORT;
            break;
        case WSA_IO_PENDING:
        case WSATRY_AGAIN:
            errno = EAGAIN;
            break;
        case WSAEWOULDBLOCK:
            errno = EWOULDBLOCK;
            break;
        case WSAEMSGSIZE:
            errno = E2BIG;
            break;
        case WSAECONNRESET:
            errno = 0;
            break;
    }
}

#define write mem_write

static inline size_t mem_write(int s, void *buf, size_t len)
{
    DWORD dwBufferCount = 0;
    int error;

    WSABUF wsabuf = { len, (char *)buf} ;
    if(WSASend(s, &wsabuf, 1, &dwBufferCount, 0, NULL, NULL) == 0) {
        return dwBufferCount;
    }
        error = WSAGetLastError();
    if(error == WSAECONNRESET) return 0;
        mapErr(error);
    return -1;
}

#define read mem_read

static inline size_t mem_read(int s, void *buf, size_t len)
{
    DWORD flags = 0;
    DWORD dwBufferCount;
    WSABUF wsabuf = { len, (char *)buf };
        int error;

    if(WSARecv((SOCKET)s,
        &wsabuf,
        1,
        &dwBufferCount,
        &flags,
        NULL,
        NULL
    ) == 0) {
        return dwBufferCount;
    }
    error = WSAGetLastError();
        if (error == WSAECONNRESET) return 0;
        mapErr(error);
    return -1;
}

static inline int sendmsg(int s, const struct msghdr *msg, int flags)
{
    DWORD dwBufferCount;
        int error;

    if(WSASendTo((SOCKET) s,
        (LPWSABUF)msg->msg_iov,
        (DWORD)msg->msg_iovlen,
        &dwBufferCount,
        flags,
        msg->msg_name,
        msg->msg_namelen,
        NULL,
        NULL
    ) == 0) {
        return dwBufferCount;
    }
    error = WSAGetLastError();
        if (error == WSAECONNRESET) return 0;
        mapErr(error);
    return -1;
}

int createLocalListSock(struct sockaddr_in *saddr);
int createLocalSocketPair(int listSock, int *fds, struct sockaddr_in *saddr);
int kill(int pid, int sig);
int sleep(int seconds);

struct sigaction {
    void (*sa_handler)(int);
    int sa_mask;
    int sa_flags;
};

#define sigemptyset(a) 0
extern int sigaction(int sig, struct sigaction *act, struct sigaction *oact);
#define daemonize(a,b) spawn_memcached(argc, argv)
extern int spawn_memcached(int argc, char **argv);
#endif

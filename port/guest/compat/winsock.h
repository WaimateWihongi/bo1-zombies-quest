/* Winsock names on top of BSD sockets (the guest's sockets are the host kernel's) */
#pragma once
#include <windows.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
typedef unsigned long u_long;
/* Winsock's ioctl codes differ from Linux's */
static inline int ioctlsocket(SOCKET s, long command, u_long *argument)
{
    int value = (int)*argument;
    if ((unsigned long)command == 0x8004667Eul) return ioctl(s, FIONBIO, &value); /* FIONBIO */
    if ((unsigned long)command == 0x4004667Ful) { /* FIONREAD */
        int r = ioctl(s, FIONREAD, &value);
        *argument = (u_long)value;
        return r;
    }
    errno = EINVAL;
    return -1;
}
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAEISCONN EISCONN
#define WSAECONNRESET ECONNRESET
typedef struct WSAData {
    WORD wVersion, wHighVersion;
    char szDescription[257], szSystemStatus[129];
    unsigned short iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;
static inline int WSAStartup(WORD version, LPWSADATA data)
{
    data->wVersion = data->wHighVersion = version;
    return 0;
}
static inline int WSACleanup(void) { return 0; }

#ifdef __cplusplus
/* Winsock takes int lengths where BSD takes socklen_t */
static inline int recvfrom(SOCKET s, char *buffer, int size, int flags, struct sockaddr *from, int *from_size)
{
    socklen_t n = (socklen_t)*from_size;
    int r = (int)recvfrom(s, buffer, (size_t)size, flags, from, &n);
    *from_size = (int)n;
    return r;
}
static inline int getsockname(SOCKET s, struct sockaddr *name, int *size)
{
    socklen_t n = (socklen_t)*size;
    int r = getsockname(s, name, &n);
    *size = (int)n;
    return r;
}
static inline int getsockopt(SOCKET s, int level, int name, char *value, int *size)
{
    socklen_t n = (socklen_t)*size;
    int r = getsockopt(s, level, name, value, &n);
    *size = (int)n;
    return r;
}
#endif

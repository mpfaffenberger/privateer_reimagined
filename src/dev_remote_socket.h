#pragma once
// -----------------------------------------------------------------------------
// dev_remote_socket.h — the handful of socket calls dev_remote.cpp needs,
// papered over for POSIX vs Winsock (#696).
//
// dev_remote keeps sockets as plain `int` fds. Winsock SOCKET is a UINT_PTR,
// but its values are small kernel handles in practice, and INVALID_SOCKET
// truncates to -1 — so the existing `fd < 0` checks keep working.
// -----------------------------------------------------------------------------

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dev_remote_socket {

// One-time stack init (WSAStartup on Windows). Returns false on failure.
inline bool startup() {
#ifdef _WIN32
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void close(int fd) {
#ifdef _WIN32
    ::closesocket((SOCKET)fd);
#else
    ::close(fd);
#endif
}

// Unblocks a thread parked in accept() so stop() can join it.
inline void shutdown_both(int fd) {
#ifdef _WIN32
    ::shutdown((SOCKET)fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

// Listen-socket options. POSIX wants SO_REUSEADDR so a restart doesn't trip
// over TIME_WAIT. On Windows SO_REUSEADDR would let a SECOND game bind the
// same port and silently steal requests, so we ask for exclusive use.
inline void set_listen_options(int fd) {
    int yes = 1;
#ifdef _WIN32
    ::setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
}

} // namespace dev_remote_socket

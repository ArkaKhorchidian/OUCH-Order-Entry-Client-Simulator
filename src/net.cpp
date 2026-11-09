#include "ouch/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctime>

namespace ouch::net {

Nanos ns_since_midnight() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    constexpr std::uint64_t kDay = 86'400ULL;
    return (static_cast<std::uint64_t>(ts.tv_sec) % kDay) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

int tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    set_nonblocking(fd);
    set_nodelay(fd);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { ::close(fd); return -1; }
    if (rc < 0) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) { ::close(fd); return -1; }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) { ::close(fd); return -1; }
    }
    return fd;
}

int tcp_listen(const std::string& host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { ::close(fd); return -1; }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 16) < 0) {
        ::close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

std::uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

bool flush(int fd, std::vector<std::uint8_t>& buf) {
    std::size_t off = 0;
    while (off < buf.size()) {
        const ssize_t n = ::send(fd, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
        if (n > 0) { off += static_cast<std::size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

long read_some(int fd, std::vector<std::uint8_t>& scratch, std::size_t max) {
    scratch.resize(max);
    for (;;) {
        const ssize_t n = ::recv(fd, scratch.data(), max, 0);
        if (n > 0) { scratch.resize(static_cast<std::size_t>(n)); return n; }
        scratch.clear();
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

} // namespace ouch::net

#pragma once

// Thin POSIX socket helpers shared by the client and the simulator.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ouch::net {

using Nanos = std::uint64_t;

[[nodiscard]] inline Nanos mono_ns() noexcept {
    return static_cast<Nanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count());
}

// OUCH timestamps: nanoseconds since midnight (UTC here).
[[nodiscard]] Nanos ns_since_midnight() noexcept;

// Returns a connected, non-blocking TCP socket with TCP_NODELAY, or -1.
[[nodiscard]] int tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms);

// Listening socket bound to `port` (0 = ephemeral). Non-blocking. -1 on error.
[[nodiscard]] int tcp_listen(const std::string& host, std::uint16_t port);
[[nodiscard]] std::uint16_t local_port(int fd);

void set_nonblocking(int fd);
void set_nodelay(int fd);
void close_fd(int& fd) noexcept;

// Non-blocking write of as much of `buf` as the socket takes; erases what was
// written. Returns false on a hard error (peer gone).
bool flush(int fd, std::vector<std::uint8_t>& buf);

// Non-blocking read into `out` (appended). Returns bytes read, 0 on EAGAIN,
// -1 on EOF or error.
long read_some(int fd, std::vector<std::uint8_t>& scratch, std::size_t max = 64 * 1024);

} // namespace ouch::net

#pragma once

// SimServer: SoupBinTCP server in front of ExchangeCore.
//
// Single-threaded poll() loop. Per connection: login handshake, then
//   client Unsequenced Data  -> ExchangeCore::handle()
//   account journal          -> Sequenced Data, from the client's next sequence
// with 1 s heartbeats each way and a 15 s receive timeout, per the spec.
//
// Sessions survive disconnects: a client that logs back in with the session
// ID and its next expected sequence number gets everything it missed, in
// order, exactly once. A new login for an account that already has a
// connection takes over (the old socket is closed), which is what a client
// reconnecting after a half-open TCP failure needs.

#include "exchange.hpp"
#include "soupbin.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ouch {

struct SimServerConfig {
    std::string   host{"127.0.0.1"};
    std::uint16_t port{0};                     // 0 = ephemeral (see port())
    std::string   session_id{"SIM0000001"};    // up to 10 chars
    std::map<std::string, std::string> users;  // username -> password; empty = accept any
    std::uint64_t heartbeat_interval_ns{1'000'000'000ULL};
    std::uint64_t receive_timeout_ns{15'000'000'000ULL};
    std::uint64_t login_timeout_ns{30'000'000'000ULL};
    ExchangeConfig exchange{};
    bool          busy_poll{false};            // spin (poll timeout 0) instead of sleeping
    bool          verbose{false};
};

class SimServer {
public:
    explicit SimServer(SimServerConfig cfg);
    ~SimServer();

    SimServer(const SimServer&) = delete;
    SimServer& operator=(const SimServer&) = delete;

    // Binds the listening socket. Returns false on failure.
    bool start();
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Run one poll iteration (timeout in ms). Single-threaded use.
    void poll_once(int timeout_ms);

    // Blocking loop until stop() (callable from another thread).
    void run();
    void stop() noexcept { running_.store(false); }

    // Convenience for tests: run() on a background thread.
    void start_background();
    void join();

    // Send End of Day + End of Session to everyone and close the sessions.
    void end_session();

    [[nodiscard]] ExchangeCore& core() noexcept { return core_; }
    [[nodiscard]] std::size_t connection_count() const noexcept { return conns_.size(); }

private:
    struct Conn;

    void accept_new();
    void on_readable(Conn& c);
    void on_packet(Conn& c, const soupbin::Packet& p);
    void on_login(Conn& c, std::span<const std::uint8_t> payload);
    void pump(Conn& c);
    void close_conn(Conn& c);

    SimServerConfig cfg_;
    ExchangeCore    core_;
    int             listen_fd_{-1};
    std::uint16_t   port_{0};
    std::vector<std::unique_ptr<Conn>> conns_;
    std::vector<std::uint8_t> scratch_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace ouch

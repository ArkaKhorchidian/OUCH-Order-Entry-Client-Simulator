// ouch_latency: order round-trip time over loopback TCP.
//
// Starts the simulator on a background thread, connects a client, and for
// each of N orders measures send(Enter Order) -> receive(Order Accepted).
// Orders are IOC and priced away from each other so the book stays empty
// and every measurement exercises the same path:
//   encode -> SoupBinTCP frame -> send() -> [loopback] -> poll/recv ->
//   reassemble -> decode -> validate -> journal -> frame -> send() ->
//   [loopback] -> recv -> reassemble -> decode -> track -> callback
//
//   ouch_latency [N] [--busy]
//
// --busy spins both the client and the server (poll timeout 0), the way a
// production gateway pins a thread to a core, instead of sleeping in poll().

#include "ouch/client.hpp"
#include "ouch/sim_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ouch;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    int n = 20'000;
    bool busy = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--busy") busy = true;
        else n = std::atoi(argv[i]);
    }

    SimServerConfig scfg;
    scfg.busy_poll = busy;
    SimServer server(scfg);
    if (!server.start()) { std::perror("listen"); return 1; }
    server.start_background();

    ClientConfig ccfg;
    ccfg.port = server.port();
    ccfg.username = "BENCH";
    OuchClient client(ccfg);

    bool accepted = false;
    client.on_message([&](const OutboundMsg& m, std::uint64_t) {
        if (std::holds_alternative<OrderAccepted>(m)) accepted = true;
    });
    if (!client.connect()) { std::fprintf(stderr, "connect failed\n"); return 1; }
    client.poll(50);

    EnterOrder o;
    o.symbol = "BENCH";
    o.quantity = 100;
    o.time_in_force = tif::IOC;

    std::vector<double> rtt;
    rtt.reserve(static_cast<std::size_t>(n));
    const int warmup = std::min(1'000, n / 10);
    for (int i = 0; i < n + warmup; ++i) {
        o.user_ref_num = 0;
        o.side = i % 2 ? side::Buy : side::Sell;
        o.price = i % 2 ? to_price(10.00) : to_price(20.00);  // never cross
        accepted = false;
        const auto t0 = Clock::now();
        client.enter_order(o);
        while (!accepted) client.poll(busy ? 0 : 1);
        const auto t1 = Clock::now();
        if (i >= warmup) rtt.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(rtt.begin(), rtt.end());
    auto pct = [&](double q) { return rtt[static_cast<std::size_t>(q * static_cast<double>(rtt.size() - 1))]; };
    std::printf("Enter Order -> Order Accepted round trip over loopback TCP, %s, n=%zu\n",
                busy ? "busy-polling" : "blocking poll()", rtt.size());
    std::printf("  p50 %.1f us   p90 %.1f us   p99 %.1f us   p99.9 %.1f us   max %.1f us\n",
                pct(0.50), pct(0.90), pct(0.99), pct(0.999), rtt.back());

    client.disconnect();
    server.stop();
    server.join();
    return 0;
}

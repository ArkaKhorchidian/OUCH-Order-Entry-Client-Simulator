// codec_bench: encode / decode cost per message, measured in batches.

#include "ouch/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace ouch;
using Clock = std::chrono::steady_clock;

template <class F>
double ns_per_op(int iters, F&& f) {
    // Best of 5 batches: the minimum is the least noisy estimate of the code's
    // own cost on a shared machine.
    double best = 1e18;
    for (int round = 0; round < 5; ++round) {
        const auto t0 = Clock::now();
        for (int i = 0; i < iters; ++i) f(i);
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
    }
    return best;
}

int main() {
    constexpr int kIters = 2'000'000;
    std::uint8_t buf[kMaxMessageSize];
    volatile std::size_t sink = 0;

    EnterOrder e;
    e.symbol = "AAPL";
    e.quantity = 100;
    e.price = to_price(187.25);
    e.cl_ord_id = "ORDER000001";

    const double enc = ns_per_op(kIters, [&](int i) {
        e.user_ref_num = static_cast<UserRefNum>(i);
        sink = sink + encode(e, buf, sizeof buf);
    });

    OrderExecuted x{34'200'000'000'000ULL, 1, 100, to_price(187.25), liquidity::Added, 1, {}};
    std::uint8_t xbuf[kMaxMessageSize];
    const std::size_t xn = encode(x, xbuf, sizeof xbuf);
    const double dec = ns_per_op(kIters, [&](int) {
        auto m = decode_outbound({xbuf, xn});
        sink = sink + (m ? 1 : 0);
    });

    OrderAccepted a;
    a.symbol = "AAPL";
    a.cl_ord_id = "ORDER000001";
    std::uint8_t abuf[kMaxMessageSize];
    const std::size_t an = encode(a, abuf, sizeof abuf);
    const double dec_a = ns_per_op(kIters, [&](int) {
        auto m = decode_outbound({abuf, an});
        sink = sink + (m ? 1 : 0);
    });

    std::printf("encode EnterOrder (47 B)      %6.1f ns\n", enc);
    std::printf("decode OrderExecuted (36 B)   %6.1f ns\n", dec);
    std::printf("decode OrderAccepted (64 B)   %6.1f ns\n", dec_a);
    return sink == 0xFFFFFFFF ? 1 : 0;
}

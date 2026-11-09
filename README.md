# OUCH Order Entry: Client and Exchange Simulator

A C++20 implementation of NASDAQ's **OUCH 5.0** order-entry protocol over
**SoupBinTCP**: a complete binary codec, a client with session recovery,
and an exchange simulator with a price-time matching engine to test it
against.

OUCH is how orders reach NASDAQ: fixed-width binary messages, big-endian
integers, no tag parsing. It's the order-entry counterpart to ITCH (market
data), and this repo pairs with my
[ITCH 5.0 parser](https://github.com/ArkaKhorchidian/fastitch): together
they cover both directions of a venue's native protocols. Where FIX is the
lingua franca between firms, binary protocols like OUCH are what latency
sensitive firms use at the venue edge.

```
 ouch_client / OuchClient                      ouch_sim / SimServer
 ┌──────────────────────┐                     ┌──────────────────────────────┐
 │ OrderTracker         │                     │ ExchangeCore                 │
 │ UserRefNum sequencer │   SoupBinTCP / TCP  │  OUCH rules + validation     │
 │ OUCH codec           │ ──Unsequenced (U)─► │  price-time matching engine  │
 │ SoupBinTCP session   │ ◄─Sequenced (S)──── │  per-account journal ────────┼─► replay on reconnect
 │  login, heartbeats,  │                     │ SoupBinTCP server            │
 │  seq tracking,       │                     │  login, heartbeats, timeouts │
 │  reconnect           │                     └──────────────────────────────┘
 └──────────────────────┘
```

## What's implemented

**OUCH 5.0 codec: every message in the spec, both directions.**

| Inbound (client → exchange) | Outbound (exchange → client) |
|---|---|
| `O` Enter Order | `S` System Event, `A` Order Accepted, `U` Order Replaced |
| `U` Replace Order | `C` Order Canceled, `D` AIQ Canceled, `E` Order Executed |
| `X` Cancel Order | `B` Broken Trade, `J` Rejected, `P` Cancel Pending |
| `M` Modify Order | `I` Cancel Reject, `T` Order Priority Update |
| `C` Mass Cancel | `M` Order Modified, `R` Order Restated |
| `D` / `E` Disable / Enable Order Entry | `X` Mass Cancel Response, `G` / `K` Disable / Enable Response |
| `Q` Account Query | `Q` Account Query Response |

Plus the optional appendage (TagValue options from Appendix A: Firm,
MinQty, MaxFloor, PostOnly, DiscretionPrice, ...), Appendix B cancel reasons
and Appendix C reject codes. Byte layouts are verified field-by-field
against the spec's offset tables in the tests.

**SoupBinTCP session layer.** 2-byte length framing with a reassembler that
handles TCP splitting and merging packets anywhere (tested byte-by-byte),
login request / accepted / rejected, sequenced and unsequenced data, 1 s
heartbeats each way, server receive timeout, logout, end of session.

**Client (`OuchClient`).**
- Login, heartbeats, silent-server detection
- Local sequence counting (SoupBinTCP doesn't put sequence numbers on the wire)
- **Reconnect with gap recovery:** logs back in with the session ID and next expected sequence number, and the server replays what was missed, exactly once, in order
- UserRefNum allocation that stays strictly increasing across reconnects, as the spec requires; after a process restart, Account Query recovers the next valid value
- `OrderTracker`: per-order open / executed / status, built only from exchange messages

**Exchange simulator (`ExchangeCore` + `SimServer`).**
- Price-time priority matching; trades at the resting price; maker/taker liquidity flags and a shared match number on both sides of each trade
- Day and IOC time in force
- OUCH semantics, not just the message shapes:
  - Cancel's quantity is the *new intended order size* (caps total executable shares, counting what already executed), not "shares to cancel"
  - A replace keeps queue priority only if the price is unchanged and size doesn't increase
  - UserRefNums at or below the last one processed are treated as retransmissions and ignored
  - Modify can only reduce size or switch between sell-side types (S ↔ T ↔ E)
- Validation with Appendix C reject codes (invalid side / quantity / symbol / price, firm disabled, ...)
- Mass cancel, disable/enable order entry, account query, end of day
- Per-account journals of every sequenced message, which double as the replay source for reconnects
- A new login for an account that's already connected takes over the session, which is what a client recovering from a half-open TCP connection needs

## Demo

```bash
./build/ouch_sim --port 15000 --user ALICE:pw1 --user BOB:pw2 --symbols AAPL,MSFT
./build/ouch_client --port 15000 --user ALICE --password pw1     # second terminal
./build/ouch_client --port 15000 --user BOB --password pw2       # third terminal
```

Or run the whole scripted scenario with `./examples/demo.sh`.

Alice rests an offer, then her connection drops. While she's offline, Bob
lifts it twice. Alice reconnects and gets both executions, in sequence:

```
=== ALICE
<- [1] SystemEvent S
-> EnterOrder ref=1
<- [2] OrderAccepted ref=1 S 500 AAPL @ 187.5000 orderref=1 state=L
connection dropped; next expected seq 3
<- [3] OrderExecuted ref=1 200 @ 187.5000 liq=A match=1      ← replayed after reconnect
<- [4] OrderExecuted ref=1 100 @ 187.5000 liq=A match=2      ← replayed after reconnect
reconnected, resumed at seq 3
  ref=1     AAPL     S open=200    exec=300    Live
<- [5] OrderCanceled ref=1 decrement=200 reason=U

=== BOB
-> EnterOrder ref=1
<- [2] OrderAccepted ref=1 B 200 AAPL @ 187.5500 orderref=2 state=L
<- [3] OrderExecuted ref=1 200 @ 187.5000 liq=R match=1      ← price improvement: trades at the resting price
-> EnterOrder ref=2
<- [4] OrderAccepted ref=2 B 100 AAPL @ 187.5000 orderref=3 state=L
<- [5] OrderExecuted ref=2 100 @ 187.5000 liq=R match=2
-> EnterOrder ref=3
<- [6] Rejected ref=3 reason=0x0017 (InvalidSymbol)
```

Client commands: `buy|sell SYMBOL QTY PRICE [ioc]`, `cancel REF [NEW_SIZE]`,
`replace REF QTY PRICE`, `query`, `orders`, `drop`, `reconnect`, `quit`.

## Using the library

```cpp
#include "ouch/client.hpp"
using namespace ouch;

ClientConfig cfg;
cfg.host = "127.0.0.1";
cfg.port = 15000;
cfg.username = "ALICE";
cfg.password = "pw1";

OuchClient client(cfg);
client.on_message([](const OutboundMsg& m, std::uint64_t seq) {
    if (auto* e = std::get_if<OrderExecuted>(&m)) { /* fill */ }
});
client.connect();

EnterOrder o;
o.side = side::Buy;
o.quantity = 100;
o.symbol = "AAPL";
o.price = to_price(187.25);
o.time_in_force = tif::Day;
UserRefNum ref = client.enter_order(o);   // UserRefNum assigned automatically

while (running) client.poll(0);           // busy-poll on a pinned thread
client.cancel_order(ref);                 // new intended size 0 = cancel all
```

The codec works standalone on byte buffers:

```cpp
std::uint8_t buf[kMaxMessageSize];
std::size_t n = encode(o, buf, sizeof buf);              // exact wire bytes
auto msg = decode_outbound({rx_bytes, rx_len});          // std::optional<OutboundMsg>
```

## Performance

Measured on a shared 2 vCPU cloud VM (Intel Xeon @ 2.30 GHz), no core
isolation.

**Codec** (`ouch_codec_bench`, best of 5 batches of 2M):

| Operation | Cost |
|---|---|
| encode Enter Order (47 B) | 11 ns |
| decode Order Executed (36 B) | 23 ns |
| decode Order Accepted (64 B) | 36 ns |

**Order round trip** (`ouch_latency`): Enter Order sent → Order Accepted
received, through the full stack on both sides (encode, frame, send,
loopback TCP, reassemble, decode, validate, journal, and back), 20,000
orders:

| Mode | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| blocking `poll()` | 41 µs | 49 µs | 75 µs | 165 µs |
| busy-polling (`--busy`) | 12 µs | 13 µs | 28 µs | 72 µs |

The gap between the two rows is the cost of putting a thread to sleep and
waking it up, twice per round trip. That's why trading gateways pin a thread
to a core and spin. What's left in the busy-polling number is mostly the
kernel TCP stack in each direction: the next step would be kernel bypass
(e.g. Solarflare/AMD `ef_vi` / OpenOnload), which is out of scope for a
loopback benchmark.

## Build and test

Requires CMake ≥ 3.20 and a C++20 compiler (GCC 12+, Clang 16+), Linux.
GoogleTest is fetched automatically. No other dependencies.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
./build/ouch_codec_bench
./build/ouch_latency 20000 --busy
```

45 tests, run under ASan + UBSan:

- **Codec:** byte-exact layout checks against the spec's offset tables, fixed sizes of all 25 messages, round trips of every message (with appendages), every truncation rejected, trailing bytes rejected, oversized unknown appendage options skipped, malformed appendage lengths rejected
- **SoupBinTCP:** login packet layouts, a stream of 52 packets reassembled identically whether fed whole, byte by byte, or in 37-byte chunks
- **Exchange:** validation codes, UserRefNum retransmission rule, price-time priority across levels, IOC, cancel-to-size semantics, replace priority rules, replace that crosses, modify, mass cancel, disable/enable, end of day, and a 5,000-order random-flow test checking share conservation (accepted = executed + canceled + resting) and an uncrossed book
- **Integration (real sockets):** order lifecycle, bad password / unknown session rejects, two clients trading, **reconnect with gap recovery exactly once**, UserRefNum recovery via Account Query after a restart, heartbeats keeping an idle session alive, the server dropping a silent client, session takeover, end of session

## Layout

```
include/ouch/
  wire.hpp         big-endian readers/writers, Alpha padding
  types.hpp        prices, enums, Appendix A/B/C codes
  messages.hpp     all OUCH 5.0 messages + codec
  soupbin.hpp      SoupBinTCP packets + stream reassembly
  exchange.hpp     OUCH semantics + matching engine (no I/O)
  sim_server.hpp   SoupBinTCP server around ExchangeCore
  client.hpp       OuchClient + OrderTracker
  net.hpp          POSIX socket helpers
tools/   ouch_sim, ouch_client
bench/   ouch_latency, codec_bench
tests/   codec, soupbin, exchange, integration
```

## Not yet

- Cross orders (opening/closing auctions), pegs, discretion and reserve (MaxFloor) behaviour in the simulator. The codec carries the fields, the matching engine ignores them
- GTX/GTT time in force and market orders
- Self-match prevention (AIQ) in the matching engine
- Persisting journals to disk so the simulator survives a restart
- A pre-trade risk hook on the client send path (see [Pre-Trade-Risk-Gateway](https://github.com/ArkaKhorchidian/Pre-Trade-Risk-Gateway))

## References

- [Nasdaq OUCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf)
- [Nasdaq SoupBinTCP specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/soupbintcp.pdf)

## License

MIT

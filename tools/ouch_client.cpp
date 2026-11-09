// ouch_client: interactive OUCH 5.0 order-entry client.
//
//   ouch_client [--host 127.0.0.1] [--port 15000] --user NAME [--password PW]
//
// Commands (stdin):
//   buy  SYMBOL QTY PRICE [ioc]
//   sell SYMBOL QTY PRICE [ioc]
//   cancel REF [NEW_SIZE]
//   replace REF QTY PRICE
//   query                 Account Query (next valid UserRefNum)
//   orders                show tracked orders
//   drop                  kill the TCP connection (simulate a network failure)
//   reconnect             log back in with session + next sequence number
//   quit

#include "ouch/client.hpp"

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace ouch;

namespace {

const char* status_name(OrderStatus s) {
    switch (s) {
        case OrderStatus::PendingNew: return "PendingNew";
        case OrderStatus::Live:       return "Live";
        case OrderStatus::Dead:       return "Dead";
        case OrderStatus::Rejected:   return "Rejected";
    }
    return "?";
}

void handle_command(OuchClient& c, const std::string& line, bool& quit) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "buy" || cmd == "sell") {
        EnterOrder m;
        std::string sym, flag;
        double px = 0;
        in >> sym >> m.quantity >> px >> flag;
        m.side = cmd == "buy" ? side::Buy : side::Sell;
        m.symbol = sym;
        m.price = to_price(px);
        m.time_in_force = flag == "ioc" ? tif::IOC : tif::Day;
        const auto ref = c.enter_order(m);
        std::printf("-> EnterOrder ref=%u\n", ref);
    } else if (cmd == "cancel") {
        UserRefNum ref = 0;
        Qty size = 0;
        in >> ref >> size;
        c.cancel_order(ref, size);
    } else if (cmd == "replace") {
        ReplaceOrder m;
        double px = 0;
        in >> m.orig_user_ref_num >> m.quantity >> px;
        m.price = to_price(px);
        std::printf("-> ReplaceOrder ref=%u\n", c.replace_order(m));
    } else if (cmd == "query") {
        c.account_query();
    } else if (cmd == "orders") {
        for (const auto& [ref, o] : c.orders().all())
            std::printf("  ref=%-5u %-8.*s %c open=%-6u exec=%-6u %s\n", ref,
                        static_cast<int>(o.symbol.len), o.symbol.data.data(), o.side, o.open, o.executed,
                        status_name(o.status));
    } else if (cmd == "drop") {
        c.drop_connection();
        std::printf("connection dropped; next expected seq %llu\n",
                    static_cast<unsigned long long>(c.next_expected_seq()));
    } else if (cmd == "reconnect") {
        const auto resume_at = c.next_expected_seq();
        if (c.reconnect())
            std::printf("reconnected, resumed at seq %llu\n", static_cast<unsigned long long>(resume_at));
        else
            std::printf("reconnect failed\n");
    } else if (cmd == "quit" || cmd == "exit") {
        quit = true;
    } else if (!cmd.empty()) {
        std::printf("unknown command: %s\n", cmd.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    ClientConfig cfg;
    cfg.port = 15000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") cfg.host = val();
        else if (a == "--port") cfg.port = static_cast<std::uint16_t>(std::atoi(val().c_str()));
        else if (a == "--user") cfg.username = val();
        else if (a == "--password") cfg.password = val();
    }
    if (cfg.username.empty()) {
        std::fprintf(stderr, "usage: ouch_client --user NAME [--password PW] [--host H] [--port N]\n");
        return 2;
    }

    OuchClient c(cfg);
    c.on_message([](const OutboundMsg& m, std::uint64_t seq) {
        std::printf("<- [%llu] %s\n", static_cast<unsigned long long>(seq), to_string(m).c_str());
        std::fflush(stdout);
    });
    if (!c.connect()) {
        std::fprintf(stderr, "login failed (reason '%c')\n", c.login_reject_reason() ? c.login_reject_reason() : '-');
        return 1;
    }
    std::printf("logged in to session %s\n", c.session().c_str());

    bool quit = false;
    std::string line;
    while (!quit) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (::poll(&p, 1, 20) > 0) {
            if (!std::getline(std::cin, line)) break;
            handle_command(c, line, quit);
        }
        if (c.state() == OuchClient::State::Active) c.poll(0);
        if (c.state() == OuchClient::State::EndOfSession) { std::printf("end of session\n"); break; }
    }
    c.disconnect();
    return 0;
}

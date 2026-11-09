// ouch_sim: standalone OUCH 5.0 exchange simulator over SoupBinTCP.
//
//   ouch_sim [--port 15000] [--host 127.0.0.1] [--user NAME:PASSWORD]...
//            [--symbols AAPL,MSFT] [--session SIM0000001] [--busy] [--quiet]
//
// Without --user, any username/password is accepted. Ctrl-C ends the session
// (End of Day + End of Session to every connected client).

#include "ouch/sim_server.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {
ouch::SimServer* g_server = nullptr;
void on_signal(int) { if (g_server) g_server->stop(); }
} // namespace

int main(int argc, char** argv) {
    ouch::SimServerConfig cfg;
    cfg.port = 15000;
    cfg.verbose = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") cfg.port = static_cast<std::uint16_t>(std::atoi(val().c_str()));
        else if (a == "--host") cfg.host = val();
        else if (a == "--session") cfg.session_id = val();
        else if (a == "--quiet") cfg.verbose = false;
        else if (a == "--busy") cfg.busy_poll = true;
        else if (a == "--user") {
            const std::string up = val();
            const auto colon = up.find(':');
            if (colon == std::string::npos) { std::fprintf(stderr, "--user expects NAME:PASSWORD\n"); return 2; }
            cfg.users[up.substr(0, colon)] = up.substr(colon + 1);
        } else if (a == "--symbols") {
            std::stringstream ss(val());
            for (std::string s; std::getline(ss, s, ',');) cfg.exchange.symbols.push_back(s);
        } else {
            std::fprintf(stderr, "usage: ouch_sim [--port N] [--host H] [--user NAME:PW]... "
                                 "[--symbols A,B] [--session ID] [--busy] [--quiet]\n");
            return 2;
        }
    }

    ouch::SimServer server(cfg);
    if (!server.start()) { std::perror("listen"); return 1; }
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("ouch_sim listening on %s:%u, session %s\n", cfg.host.c_str(), server.port(),
                cfg.session_id.c_str());
    server.run();
    server.end_session();
    const auto& s = server.core().stats();
    std::printf("\naccepted %llu, rejected %llu, executions %llu\n",
                static_cast<unsigned long long>(s.orders_accepted),
                static_cast<unsigned long long>(s.orders_rejected),
                static_cast<unsigned long long>(s.executions));
    return 0;
}

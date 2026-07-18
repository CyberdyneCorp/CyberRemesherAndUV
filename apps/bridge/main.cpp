// Reference bridge host: a minimal process that stands up a BridgeServer on a
// local port and serves until stdin closes (or a "quit" line arrives). The
// real application embeds BridgeServer in its document/session layer (group
// 8); this harness exists so scripts and the Python integration tests have a
// server to talk to. It prints "LISTENING <port>" once bound.
#include <cstdint>
#include <iostream>
#include <string>

#include "cyber/net/server.hpp"
#include "cyber/net/session.hpp"

int main(int argc, char** argv) {
    std::uint16_t port = 0;  // 0 -> ephemeral local port
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::stoi(std::string(argv[1])));
    }

    cyber::net::BridgeSession session;
    cyber::net::BridgeServer server(session);
    if (!server.start(port)) {
        std::cerr << "failed to bind local bridge port\n";
        return 1;
    }
    std::cout << "LISTENING " << server.port() << "\n";
    std::cout.flush();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") {
            break;
        }
    }
    server.stop();
    return 0;
}

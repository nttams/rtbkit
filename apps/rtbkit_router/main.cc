// The core server as a standalone process: banker, router, OpenRTB exchange
// connector, post auction service, ad server and a small admin HTTP endpoint.
// Bidding agents (fixed_price_agent) connect over zmq, an exchange (or
// mock_exchange) posts OpenRTB requests to the connector.
//
//   rtbkit_router [--config config/rtbkit.json] [--stats-interval SECONDS]
//
// See config/rtbkit.json and src/server/core_server.h for the settings.
// Admin: GET http://127.0.0.1:12370/stats. SIGINT / SIGTERM stop it cleanly.

#include "server/core_server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace RTBKIT;

namespace {

std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested = true; }

} // namespace

int main(int argc, char** argv)
{
    try {
        std::string configPath = "config/rtbkit.json";
        int statsInterval = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--config") configPath = next();
            else if (arg == "--stats-interval") statsInterval = std::stoi(next());
            else throw std::runtime_error(
                "unknown argument " + arg
                + "\nusage: rtbkit_router [--config file] [--stats-interval seconds]");
        }

        std::ifstream in(configPath);
        if (!in) throw std::runtime_error("cannot read config " + configPath);
        CoreServer server(nlohmann::json::parse(in));

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        server.start();
        std::cout << "rtbkit_router: agents " << server.agentEndpoint()
                  << ", exchange http://127.0.0.1:" << server.exchangePort() << "/auctions"
                  << ", adserver http://127.0.0.1:" << server.adServerPort()
                  << ", admin http://127.0.0.1:" << server.adminPort() << "/stats"
                  << std::endl;

        auto last = std::chrono::steady_clock::now();
        while (!stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            if (statsInterval > 0 && now - last >= std::chrono::seconds(statsInterval)) {
                last = now;
                std::cout << server.statsJson()["router"].dump() << std::endl;
            }
        }

        server.stop();
        std::cout << "rtbkit_router: stopped\n" << server.statsJson().dump(2) << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "rtbkit_router: " << e.what() << std::endl;
        return 1;
    }
}

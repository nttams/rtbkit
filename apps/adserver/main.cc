// Standalone ad server: PostAuctionService (NullBanker) + AdServer, printing
// the post auction stats every few seconds. Without a router nothing is ever
// submitted, so every win is buffered and then counted as unmatched; this is
// mostly for trying the HTTP endpoints. The demo embeds the same two classes
// in-process next to the router and the real banker.
//
//   adserver [--config file.json] [--port N] [--stats-interval SECONDS]
//   config: {"adserver": {"port": 12360, "host": "127.0.0.1", "threads": 8},
//            "postAuction": {"earlyWinTimeoutMs": 10000,
//                            "finishedWindowMs": 600000,
//                            "lostWindowMs": 600000, "tickIntervalMs": 100,
//                            "maxEntries": 1000000},
//            "statsIntervalSeconds": 5}
// SIGINT / SIGTERM stop it cleanly.

#include "adserver/adserver.h"
#include "post_auction/post_auction.h"

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

PostAuctionService::Options readOptions(const nlohmann::json& j)
{
    PostAuctionService::Options options;
    auto read = [&](const char* name, std::chrono::milliseconds& out) {
        if (j.contains(name)) out = std::chrono::milliseconds(j.at(name).get<int64_t>());
    };
    read("earlyWinTimeoutMs", options.earlyWinTimeout);
    read("finishedWindowMs", options.finishedWindow);
    read("lostWindowMs", options.lostWindow);
    read("tickIntervalMs", options.tickInterval);
    if (j.contains("maxEntries")) options.maxEntries = j.at("maxEntries").get<size_t>();
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        nlohmann::json config = nlohmann::json::object();
        int port = -1;
        int statsInterval = -1;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--config") {
                std::ifstream in(next());
                if (!in) throw std::runtime_error("cannot read config file");
                config = nlohmann::json::parse(in);
            }
            else if (arg == "--port") port = std::stoi(next());
            else if (arg == "--stats-interval") statsInterval = std::stoi(next());
            else throw std::runtime_error("unknown argument " + arg);
        }

        nlohmann::json adConfig = config.value("adserver", nlohmann::json::object());
        if (port >= 0) adConfig["port"] = port;
        if (statsInterval < 0) statsInterval = config.value("statsIntervalSeconds", 5);

        PostAuctionService postAuction(
            std::make_shared<NullBanker>(),
            readOptions(config.value("postAuction", nlohmann::json::object())));
        AdServer adServer(postAuction);
        adServer.configure(adConfig);

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        postAuction.start();
        adServer.start();
        std::cout << "adserver listening on port " << adServer.port() << std::endl;

        auto lastStats = std::chrono::steady_clock::now();
        while (!stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto now = std::chrono::steady_clock::now();
            if (statsInterval > 0 && now - lastStats >= std::chrono::seconds(statsInterval)) {
                lastStats = now;
                std::cout << postAuction.statsJson().dump() << std::endl;
            }
        }

        adServer.stop();
        postAuction.stop();
        std::cout << "adserver stopped: " << postAuction.statsJson().dump() << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "adserver: " << e.what() << std::endl;
        return 1;
    }
}

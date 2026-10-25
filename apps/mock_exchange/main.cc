// Mock exchange: load generator for the OpenRTB exchange connector.
//
//   mock_exchange --url http://127.0.0.1:12350/auctions --qps 200 --count 1000
//                 --win-rate 0.5 --adserver http://127.0.0.1:12360
//
// Posts random OpenRTB requests at a target rate, reads the bid responses and
// calls the win notice url of each bid (with ${AUCTION_PRICE} replaced) for a
// share of them. With --adserver it also calls /impression after a win and
// sometimes /click. Prints a summary at the end.

#include "exchange/mock_exchange_support.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace RTBKIT;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string url = "http://127.0.0.1:12350/auctions";
    std::string adserver;            // base url, empty = no impression / click
    double qps = 100;
    int64_t count = -1;              // -1 = not set
    double durationSec = 0;          // 0 = not set
    double winRate = 0.5;
    double clickRate = 0.1;
    int threads = 16;
    int tmaxMs = 100;
    int timeoutMs = 2000;
    uint64_t seed = 0;
};

void usage()
{
    std::cout <<
        "usage: mock_exchange [options]\n"
        "  --url <url>         connector url (default http://127.0.0.1:12350/auctions)\n"
        "  --qps <n>           target requests per second (default 100)\n"
        "  --count <n>         number of requests (default 1000, or unlimited with --duration)\n"
        "  --duration <sec>    stop after this many seconds\n"
        "  --win-rate <0..1>   share of bids whose win notice is called (default 0.5)\n"
        "  --adserver <url>    ad server base url: call /impression after a win, sometimes /click\n"
        "  --click-rate <0..1> share of impressions that get a click (default 0.1)\n"
        "  --threads <n>       sender threads (default 16)\n"
        "  --tmax <ms>         tmax in the requests (default 100)\n"
        "  --timeout <ms>      HTTP timeout (default 2000)\n"
        "  --seed <n>          random seed (default: random)\n";
}

bool parseArgs(int argc, char** argv, Options& o)
{
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--url") o.url = value();
            else if (arg == "--adserver") o.adserver = value();
            else if (arg == "--qps") o.qps = std::stod(value());
            else if (arg == "--count") o.count = std::stoll(value());
            else if (arg == "--duration") o.durationSec = std::stod(value());
            else if (arg == "--win-rate") o.winRate = std::stod(value());
            else if (arg == "--click-rate") o.clickRate = std::stod(value());
            else if (arg == "--threads") o.threads = std::stoi(value());
            else if (arg == "--tmax") o.tmaxMs = std::stoi(value());
            else if (arg == "--timeout") o.timeoutMs = std::stoi(value());
            else if (arg == "--seed") o.seed = std::stoull(value());
            else if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
            else throw std::invalid_argument("unknown option " + arg);
        }
        if (o.qps <= 0 || o.threads < 1 || o.winRate < 0 || o.winRate > 1
            || o.clickRate < 0 || o.clickRate > 1 || o.tmaxMs < 1 || o.timeoutMs < 1
            || o.durationSec < 0)
            throw std::invalid_argument("option value out of range");
        if (o.count < 0 && o.durationSec == 0) o.count = 1000;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "mock_exchange: " << e.what() << "\n";
        usage();
        return false;
    }
}

struct Totals {
    uint64_t sent = 0, bidResponses = 0, bids = 0, noBids = 0, errors = 0;
    uint64_t winsNotified = 0, winErrors = 0, impressions = 0, clicks = 0;
    std::vector<double> latenciesMs;

    void add(const Totals& o)
    {
        sent += o.sent; bidResponses += o.bidResponses; bids += o.bids;
        noBids += o.noBids; errors += o.errors; winsNotified += o.winsNotified;
        winErrors += o.winErrors; impressions += o.impressions; clicks += o.clicks;
        latenciesMs.insert(latenciesMs.end(), o.latenciesMs.begin(), o.latenciesMs.end());
    }
};

/** One keep-alive client per origin. */
class Clients {
public:
    explicit Clients(int timeoutMs) : timeoutMs_(timeoutMs) {}

    httplib::Client& get(const std::string& origin)
    {
        auto& client = clients_[origin];
        if (!client) {
            client = std::make_unique<httplib::Client>(origin);
            client->set_connection_timeout(0, timeoutMs_ * 1000);
            client->set_read_timeout(timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000);
            client->set_keep_alive(true);
        }
        return *client;
    }

    /** GET a full url or an origin + path. True on a 2xx answer. */
    bool getOk(const std::string& url)
    {
        try {
            auto [origin, path] = splitUrl(url);
            auto res = get(origin).Get(path);
            return res && res->status >= 200 && res->status < 300;
        }
        catch (const std::invalid_argument&) {
            return false;
        }
    }

private:
    int timeoutMs_;
    std::map<std::string, std::unique_ptr<httplib::Client>> clients_;
};

void handleBid(const json& bid, const std::string& auctionId, const Options& o,
               std::mt19937_64& rng, Clients& clients, Totals& t)
{
    ++t.bids;
    std::uniform_real_distribution<double> unit(0, 1);
    if (unit(rng) >= o.winRate) return;
    if (!bid.contains("nurl") || !bid["nurl"].is_string() || !bid.contains("price")
        || !bid["price"].is_number())
        return;

    // Win at the bid price half of the time, otherwise at 50-100% of it
    // (second price auctions clear below the bid).
    double price = bid["price"].get<double>();
    if (unit(rng) < 0.5) price *= 0.5 + 0.5 * unit(rng);

    std::string nurl = replaceAuctionPrice(bid["nurl"].get<std::string>(), price);
    if (!clients.getOk(nurl)) { ++t.winErrors; return; }
    ++t.winsNotified;

    if (o.adserver.empty()) return;
    std::string spot = queryParam(nurl, "adSpotId");
    std::string id = queryParam(nurl, "auctionId");
    if (id.empty()) id = auctionId;
    std::string query = "?auctionId=" + urlEncode(id) + "&adSpotId=" + urlEncode(spot);

    if (clients.getOk(o.adserver + "/impression" + query)) ++t.impressions;
    if (unit(rng) < o.clickRate && clients.getOk(o.adserver + "/click" + query)) ++t.clicks;
}

void worker(int index, const Options& o, std::atomic<int64_t>& next, Clock::time_point start,
            Clock::time_point end, Totals& t)
{
    std::mt19937_64 rng(o.seed + uint64_t(index) * 7919);
    MockRequestOptions reqOptions;
    reqOptions.tmaxMs = o.tmaxMs;

    auto [origin, path] = splitUrl(o.url);
    Clients clients(o.timeoutMs);

    while (true) {
        int64_t n = next.fetch_add(1);
        if (o.count >= 0 && n >= o.count) break;
        auto due = start + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(double(n) / o.qps));
        if (o.durationSec > 0 && due >= end) break;
        std::this_thread::sleep_until(due);

        json request = generateOpenRtbRequest(rng, reqOptions);
        std::string auctionId = request["id"].get<std::string>();

        auto sentAt = Clock::now();
        auto res = clients.get(origin).Post(path, request.dump(), "application/json");
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - sentAt).count();
        ++t.sent;
        t.latenciesMs.push_back(ms);

        if (!res) { ++t.errors; continue; }
        if (res->status == 204) { ++t.noBids; continue; }
        if (res->status != 200) { ++t.errors; continue; }

        json response = json::parse(res->body, nullptr, false);
        if (!response.is_object() || !response.contains("seatbid")
            || !response["seatbid"].is_array()) {
            ++t.errors;
            continue;
        }
        ++t.bidResponses;
        for (const auto& seat : response["seatbid"])
            for (const auto& bid : seat.value("bid", json::array()))
                handleBid(bid, auctionId, o, rng, clients, t);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;
    if (o.seed == 0) o.seed = std::random_device{}();

    try {
        splitUrl(o.url);
    }
    catch (const std::invalid_argument& e) {
        std::cerr << "mock_exchange: " << e.what() << "\n";
        return 2;
    }

    std::cout << "mock_exchange -> " << o.url << " qps=" << o.qps
              << " threads=" << o.threads << " seed=" << o.seed << std::endl;

    std::atomic<int64_t> next{0};
    auto start = Clock::now() + std::chrono::milliseconds(50);
    auto end = start + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(o.durationSec));

    std::vector<Totals> totals(size_t(o.threads));
    std::vector<std::thread> threads;
    for (int i = 0; i < o.threads; ++i)
        threads.emplace_back(worker, i, std::cref(o), std::ref(next), start, end,
                             std::ref(totals[size_t(i)]));
    for (auto& t : threads) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    Totals all;
    for (const auto& t : totals) all.add(t);

    std::printf("sent            %llu (%.1f req/s over %.2f s)\n",
                (unsigned long long)all.sent, elapsed > 0 ? double(all.sent) / elapsed : 0.0,
                elapsed);
    std::printf("bid responses   %llu\n", (unsigned long long)all.bidResponses);
    std::printf("bids            %llu\n", (unsigned long long)all.bids);
    std::printf("no-bids         %llu\n", (unsigned long long)all.noBids);
    std::printf("errors          %llu\n", (unsigned long long)all.errors);
    std::printf("latency p50     %.2f ms\n", percentile(all.latenciesMs, 0.50));
    std::printf("latency p99     %.2f ms\n", percentile(all.latenciesMs, 0.99));
    std::printf("wins notified   %llu (failed %llu)\n",
                (unsigned long long)all.winsNotified, (unsigned long long)all.winErrors);
    if (!o.adserver.empty())
        std::printf("impressions     %llu\nclicks          %llu\n",
                    (unsigned long long)all.impressions, (unsigned long long)all.clicks);

    return all.sent > 0 && all.errors == all.sent ? 1 : 0;
}

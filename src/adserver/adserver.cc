#include "adserver/adserver.h"

#include <httplib.h>

#include <sys/socket.h>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace RTBKIT {

struct AdServer::Impl {
    httplib::Server server;
    std::thread thread;
    int boundPort = 0;
};

namespace {

/** Reads a required non-empty query parameter; false (and no change) if it is
    missing. */
bool param(const httplib::Request& request, const char* name, std::string& out)
{
    if (!request.has_param(name)) return false;
    std::string value = request.get_param_value(name);
    if (value.empty()) return false;
    out = std::move(value);
    return true;
}

void badRequest(httplib::Response& response, const std::string& why)
{
    response.status = 400;
    response.set_content(why, "text/plain");
}

/** Fills the ids; false (and a 400) if one is missing. */
bool readIds(const httplib::Request& request, httplib::Response& response,
             PostAuctionEvent& event)
{
    std::string auctionId, adSpotId;
    if (!param(request, "auctionId", auctionId)) {
        badRequest(response, "missing parameter: auctionId");
        return false;
    }
    if (!param(request, "adSpotId", adSpotId)) {
        badRequest(response, "missing parameter: adSpotId");
        return false;
    }
    event.auctionId = Id(std::move(auctionId));
    event.adSpotId = Id(std::move(adSpotId));
    event.timestamp = std::chrono::system_clock::now();
    return true;
}

} // namespace

AdServer::AdServer(PostAuctionService& postAuction)
    : postAuction_(postAuction), impl_(std::make_unique<Impl>())
{
}

AdServer::~AdServer()
{
    stop();
}

void AdServer::configure(const nlohmann::json& config)
{
    if (!config.is_object()) throw std::runtime_error("adserver config must be an object");
    try {
        if (config.contains("host")) host_ = config.at("host").get<std::string>();
        if (config.contains("port")) {
            int port = config.at("port").get<int>();
            if (port < 0 || port > 65535) throw std::runtime_error("port out of range");
            port_ = port;
        }
        if (config.contains("threads")) {
            int threads = config.at("threads").get<int>();
            if (threads < 1) throw std::runtime_error("threads must be at least 1");
            threads_ = size_t(threads);
        }
    }
    catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("bad adserver config: ") + e.what());
    }
    if (impl_->thread.joinable()) throw std::runtime_error("adserver already started");
}

void AdServer::start()
{
    if (impl_->thread.joinable()) return;

    // A fresh server each time, so stop() then start() works.
    impl_ = std::make_unique<Impl>();
    httplib::Server& server = impl_->server;
    const size_t threads = threads_;
    server.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };

    // httplib also sets SO_REUSEPORT on some systems, which lets two servers
    // bind the same port silently. Only reuse the address (quick restarts).
    server.set_socket_options([](socket_t sock) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));
    });

    PostAuctionService& postAuction = postAuction_;

    server.Get("/ping", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("pong", "text/plain");
    });

    server.Get("/stats", [&postAuction](const httplib::Request&, httplib::Response& response) {
        response.set_content(postAuction.statsJson().dump(), "application/json");
    });

    server.Get("/win", [&postAuction](const httplib::Request& request,
                                      httplib::Response& response) {
        PostAuctionEvent event;
        event.type = PostAuctionEventType::WIN;
        if (!readIds(request, response, event)) return;

        std::string price;
        if (!param(request, "price", price)) {
            badRequest(response, "missing parameter: price");
            return;
        }
        try {
            size_t used = 0;
            double cpm = std::stod(price, &used);
            if (used != price.size() || !std::isfinite(cpm) || cpm < 0) {
                throw std::invalid_argument("bad price");
            }
            CurrencyCode currency = CurrencyCode::USD;
            std::string code;
            if (param(request, "currency", code)) currency = parseCurrencyCode(code);
            event.winPrice = Amount::fromCpm(currency, cpm);
        }
        catch (const std::exception&) {
            badRequest(response, "bad parameter: price or currency");
            return;
        }
        postAuction.onEvent(event);
        response.set_content("ok", "text/plain");
    });

    auto campaign = [&postAuction](CampaignEventType type) {
        return [&postAuction, type](const httplib::Request& request,
                                    httplib::Response& response) {
            PostAuctionEvent event;
            event.type = PostAuctionEventType::CAMPAIGN_EVENT;
            event.campaignEvent = type;
            if (!readIds(request, response, event)) return;
            postAuction.onEvent(event);
            response.set_content("ok", "text/plain");
        };
    };
    server.Get("/impression", campaign(CampaignEventType::IMPRESSION));
    server.Get("/click", campaign(CampaignEventType::CLICK));

    int bound = port_;
    if (port_ == 0) {
        bound = server.bind_to_any_port(host_);
    }
    else if (!server.bind_to_port(host_, port_)) {
        bound = -1;
    }
    if (bound <= 0) {
        throw std::runtime_error("adserver cannot listen on " + host_ + ":" +
                                 std::to_string(port_));
    }
    impl_->boundPort = bound;
    impl_->thread = std::thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
}

void AdServer::stop()
{
    if (!impl_->thread.joinable()) return;
    impl_->server.stop();
    impl_->thread.join();
}

int AdServer::port() const
{
    return impl_->boundPort > 0 ? impl_->boundPort : port_;
}

} // namespace RTBKIT

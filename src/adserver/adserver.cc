#include "adserver/adserver.h"

#include "http/http_server.h"

#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace RTBKIT {

struct AdServer::Impl {
    HttpServer server;
};

namespace {

/** A required non-empty query parameter (URL-decoded); nullopt if missing. */
std::optional<std::string> param(const HttpRequest& request, const char* name)
{
    auto value = request.param(name);
    if (!value || value->empty()) return std::nullopt;
    return value;
}

HttpResponse badRequest(const std::string& why) { return {400, why, "text/plain"}; }

/** Fills the ids; the error answer if one is missing. */
std::optional<HttpResponse> readIds(const HttpRequest& request, PostAuctionEvent& event)
{
    auto auctionId = param(request, "auctionId");
    if (!auctionId) return badRequest("missing parameter: auctionId");
    auto adSpotId = param(request, "adSpotId");
    if (!adSpotId) return badRequest("missing parameter: adSpotId");
    event.auctionId = Id(std::move(*auctionId));
    event.adSpotId = Id(std::move(*adSpotId));
    event.timestamp = std::chrono::system_clock::now();
    return std::nullopt;
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
    if (impl_->server.running()) throw std::runtime_error("adserver already started");
}

void AdServer::start()
{
    if (impl_->server.running()) return;

    // A fresh server each time, so stop() then start() works.
    impl_ = std::make_unique<Impl>();
    HttpServer& server = impl_->server;
    PostAuctionService& postAuction = postAuction_;

    // Every handler answers at once: onEvent only queues work.
    server.route("GET", "/ping", [](const HttpRequest&, HttpResponder r) {
        r.send({200, "pong"});
    });

    server.route("GET", "/stats", [&postAuction](const HttpRequest&, HttpResponder r) {
        r.send({200, postAuction.statsJson().dump(), "application/json"});
    });

    server.route("GET", "/win", [&postAuction](const HttpRequest& request, HttpResponder r) {
        PostAuctionEvent event;
        event.type = PostAuctionEventType::WIN;
        if (auto error = readIds(request, event)) {
            r.send(std::move(*error));
            return;
        }
        auto price = param(request, "price");
        if (!price) {
            r.send(badRequest("missing parameter: price"));
            return;
        }
        try {
            size_t used = 0;
            double cpm = std::stod(*price, &used);
            if (used != price->size() || !std::isfinite(cpm) || cpm < 0) {
                throw std::invalid_argument("bad price");
            }
            CurrencyCode currency = CurrencyCode::USD;
            if (auto code = param(request, "currency")) currency = parseCurrencyCode(*code);
            event.winPrice = Amount::fromCpm(currency, cpm);
        }
        catch (const std::exception&) {
            r.send(badRequest("bad parameter: price or currency"));
            return;
        }
        postAuction.onEvent(event);
        r.send({200, "ok"});
    });

    auto campaign = [&postAuction](CampaignEventType type) {
        return [&postAuction, type](const HttpRequest& request, HttpResponder r) {
            PostAuctionEvent event;
            event.type = PostAuctionEventType::CAMPAIGN_EVENT;
            event.campaignEvent = type;
            if (auto error = readIds(request, event)) {
                r.send(std::move(*error));
                return;
            }
            postAuction.onEvent(event);
            r.send({200, "ok"});
        };
    };
    server.route("GET", "/impression", campaign(CampaignEventType::IMPRESSION));
    server.route("GET", "/click", campaign(CampaignEventType::CLICK));

    HttpServer::Options options;
    options.host = host_;
    options.port = port_;
    options.threads = int(threads_);
    options.name = "adserver";
    try {
        server.start(options);
    }
    catch (const std::exception&) {
        throw std::runtime_error("adserver cannot listen on " + host_ + ":" +
                                 std::to_string(port_));
    }
}

void AdServer::stop()
{
    impl_->server.stop();
}

int AdServer::port() const
{
    return impl_->server.running() ? impl_->server.port() : port_;
}

} // namespace RTBKIT

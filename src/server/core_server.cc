#include "server/core_server.h"

#include "banker/banker_factory.h"

#include "http/http_server.h"

#include <chrono>
#include <stdexcept>

namespace RTBKIT {

using nlohmann::json;

namespace {

json section(const json& config, const char* name)
{
    auto it = config.find(name);
    if (it == config.end() || it->is_null()) return json::object();
    if (!it->is_object())
        throw std::runtime_error(std::string("core server: \"") + name
                                 + "\" must be an object");
    return *it;
}

PostAuctionService::Options postAuctionOptions(const json& j)
{
    PostAuctionService::Options options;
    auto read = [&](const char* name, std::chrono::milliseconds& out) {
        auto it = j.find(name);
        if (it == j.end()) return;
        if (!it->is_number_integer())
            throw std::runtime_error(std::string("core server: postAuction.") + name
                                     + " must be an integer");
        out = std::chrono::milliseconds(it->get<int64_t>());
    };
    read("earlyWinTimeoutMs", options.earlyWinTimeout);
    read("finishedWindowMs", options.finishedWindow);
    read("lostWindowMs", options.lostWindow);
    read("tickIntervalMs", options.tickInterval);
    if (auto it = j.find("maxEntries"); it != j.end()) {
        if (!it->is_number_unsigned() || it->get<uint64_t>() == 0)
            throw std::runtime_error(
                "core server: postAuction.maxEntries must be a positive integer");
        options.maxEntries = it->get<size_t>();
    }
    return options;
}

} // namespace

CoreServer::CoreServer(const json& config)
{
    if (!config.is_object())
        throw std::runtime_error("core server: config must be an object");

    banker_ = makeBanker(section(config, "banker"));
    postAuction_ = std::make_unique<PostAuctionService>(
        banker_, postAuctionOptions(section(config, "postAuction")));
    adServer_ = std::make_unique<AdServer>(*postAuction_);
    adServer_->configure(section(config, "adserver"));
    router_ = std::make_unique<Router>(RouterConfig::fromJson(section(config, "router")),
                                       banker_);
    connector_ = std::make_shared<OpenRtbExchangeConnector>();
    exchangeConfig_ = section(config, "exchange");
    if (config.contains("admin")) {
        adminConfig_ = section(config, "admin");
        if (!adminConfig_.contains("port")) adminConfig_["port"] = 12370;
    }

    // router -> post auction -> router
    router_->setSubmittedAuctionSink(
        [pa = postAuction_.get()](const SubmittedAuctionEvent& e) { pa->onSubmittedAuction(e); });
    postAuction_->setWinLossHandler(
        [r = router_.get()](const MatchedWinLoss& e) { r->onMatchedWinLoss(e); });
    postAuction_->setCampaignEventHandler(
        [r = router_.get()](const MatchedCampaignEvent& e) { r->onMatchedCampaignEvent(e); });
}

CoreServer::~CoreServer()
{
    stop();
}

void CoreServer::start()
{
    if (started_) return;
    try {
        postAuction_->start();
        adServer_->start();

        json exchange = exchangeConfig_;
        if (!exchange.contains("winNoticeUrl"))
            exchange["winNoticeUrl"] = "http://127.0.0.1:"
                + std::to_string(adServer_->port()) + "/win";
        connector_->configure(exchange);
        router_->addExchange(connector_);

        router_->start();
        connector_->start();
        if (!adminConfig_.is_null()) startAdmin();
        started_ = true;
    }
    catch (...) {
        stopAll();
        throw;
    }
}

void CoreServer::startAdmin()
{
    auto server = std::make_unique<HttpServer>();
    server->route("GET", "/stats", [this](const HttpRequest&, HttpResponder r) {
        r.send({200, statsJson().dump(2), "application/json"});
    });
    server->route("GET", "/ping", [](const HttpRequest&, HttpResponder r) {
        r.send({200, "pong"});
    });
    HttpServer::Options options;
    options.host = adminConfig_.value("host", std::string("127.0.0.1"));
    options.port = adminConfig_.value("port", 12370);
    options.threads = 1;
    options.name = "core server admin";
    try {
        server->start(options);
    }
    catch (const std::exception&) {
        throw std::runtime_error("core server: admin can not listen on " + options.host + ":"
                                 + std::to_string(options.port));
    }
    adminPort_ = server->port();
    admin_ = std::move(server);
}

void CoreServer::stop()
{
    stopAll();
    started_ = false;
}

void CoreServer::stopAll()
{
    if (admin_) {
        admin_->stop();
        admin_.reset();
        adminPort_ = 0;
    }
    connector_->stop();
    adServer_->stop();
    // The router's stop path still submits the winners it holds, so the post
    // auction service must outlive it; then whatever is still waiting for a
    // result is released at the banker (it would stay reserved forever).
    router_->stop();
    postAuction_->flushPending();
    postAuction_->stop();
}

json CoreServer::statsJson() const
{
    const auto c = connector_->stats();
    return json{{"router", router_->stats()},
                {"connector", {{"requests", c.requests},
                               {"bids", c.bids},
                               {"noBids", c.noBids},
                               {"timeouts", c.timeouts},
                               {"expiredInQueue", c.expiredInQueue},
                               {"badRequests", c.badRequests},
                               {"errors", c.errors}}},
                {"postAuction", postAuction_->statsJson()},
                {"banker", banker_->summaryJson()}};
}

} // namespace RTBKIT

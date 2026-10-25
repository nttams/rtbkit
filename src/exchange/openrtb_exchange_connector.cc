#include "exchange/openrtb_exchange_connector.h"

#include "agent/agent_config.h"
#include "common/openrtb_parser.h"
#include "exchange/mock_exchange_support.h"

#include <httplib.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace RTBKIT {

using nlohmann::json;

namespace {

/** What a creative's providerConfig.<exchange> says. */
struct CreativeData {
    std::string adm;
    std::string crid;
    std::vector<std::string> adomain;
};

/** Reads providerConfig.<exchange>; nullopt if it has bad types. */
std::optional<CreativeData> readCreativeData(const Creative& creative,
                                             const std::string& exchange)
{
    CreativeData data;
    if (!creative.providerConfig.is_object()) return data;
    auto it = creative.providerConfig.find(exchange);
    if (it == creative.providerConfig.end() || it->is_null()) return data;
    if (!it->is_object()) return std::nullopt;

    if (auto adm = it->find("adm"); adm != it->end() && !adm->is_null()) {
        if (!adm->is_string()) return std::nullopt;
        data.adm = adm->get<std::string>();
    }
    if (auto crid = it->find("crid"); crid != it->end() && !crid->is_null()) {
        if (crid->is_string()) data.crid = crid->get<std::string>();
        else if (crid->is_number_integer()) data.crid = std::to_string(crid->get<int64_t>());
        else return std::nullopt;
    }
    if (auto adv = it->find("adomain"); adv != it->end() && !adv->is_null()) {
        if (!adv->is_array()) return std::nullopt;
        for (const auto& d : *adv) {
            if (!d.is_string()) return std::nullopt;
            data.adomain.push_back(d.get<std::string>());
        }
    }
    return data;
}

template<typename T>
T setting(const json& config, const char* key, T fallback)
{
    auto it = config.find(key);
    if (it == config.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    }
    catch (const json::exception&) {
        throw std::runtime_error(std::string("openrtb connector: bad value for \"")
                                 + key + "\"");
    }
}

json errorJson(const std::string& message) { return json{{"error", message}}; }

struct Waiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

} // namespace

OpenRtbExchangeConnector::OpenRtbExchangeConnector(std::string name)
    : ExchangeConnector(std::move(name))
{
}

OpenRtbExchangeConnector::~OpenRtbExchangeConnector()
{
    stop();
}

void OpenRtbExchangeConnector::configure(const json& config)
{
    if (!config.is_object() && !config.is_null())
        throw std::runtime_error("openrtb connector: config must be an object");

    std::string name = setting<std::string>(config, "name", exchangeName());
    if (name.empty()) throw std::runtime_error("openrtb connector: empty name");
    host_ = setting<std::string>(config, "host", host_);
    int port = setting<int>(config, "port", configuredPort_);
    if (port < 0 || port > 65535)
        throw std::runtime_error("openrtb connector: bad port");
    int threads = setting<int>(config, "threads", threads_);
    if (threads < 1) throw std::runtime_error("openrtb connector: threads must be >= 1");
    std::string winUrl = setting<std::string>(config, "winNoticeUrl", winNoticeUrl_);
    if (winUrl.empty()) throw std::runtime_error("openrtb connector: empty winNoticeUrl");
    double maxTmax = setting<double>(config, "maxTmaxMs", maxTmaxMs_);
    double grace = setting<double>(config, "graceMs", graceMs_);
    if (maxTmax <= 0 || grace < 0)
        throw std::runtime_error("openrtb connector: bad maxTmaxMs or graceMs");

    setExchangeName(name);
    configuredPort_ = port;
    threads_ = threads;
    winNoticeUrl_ = winUrl;
    maxTmaxMs_ = maxTmax;
    graceMs_ = grace;
}

void OpenRtbExchangeConnector::start()
{
    if (server_) return;

    auto server = std::make_unique<httplib::Server>();
    server->new_task_queue = [n = size_t(threads_)] { return new httplib::ThreadPool(n); };

    server->Post("/auctions", [this](const httplib::Request& req, httplib::Response& res) {
        std::string body;
        int status = 500;
        handleAuction(req.body, body, status);
        res.status = status;
        if (status != 204) res.set_content(body, "application/json");
    });
    server->Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    int bound = configuredPort_ == 0
        ? server->bind_to_any_port(host_)
        : (server->bind_to_port(host_, configuredPort_) ? configuredPort_ : -1);
    if (bound < 0)
        throw std::runtime_error("openrtb connector: can not listen on " + host_ + ":"
                                 + std::to_string(configuredPort_));

    port_ = bound;
    server_ = std::move(server);
    thread_ = std::thread([s = server_.get()] { s->listen_after_bind(); });
    server_->wait_until_ready();
}

void OpenRtbExchangeConnector::stop()
{
    if (!server_) return;
    server_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
    port_ = 0;
}

OpenRtbExchangeConnector::Stats OpenRtbExchangeConnector::stats() const
{
    Stats s;
    s.requests = requests_;
    s.bids = bids_;
    s.noBids = noBids_;
    s.timeouts = timeouts_;
    s.badRequests = badRequests_;
    s.errors = errors_;
    return s;
}

std::shared_ptr<void> OpenRtbExchangeConnector::getCreativeProviderData(
        const AgentConfig&, const Creative& creative) const
{
    auto data = readCreativeData(creative, exchangeName());
    if (!data) return nullptr;
    return std::make_shared<CreativeData>(std::move(*data));
}

void OpenRtbExchangeConnector::handleAuction(const std::string& body,
                                             std::string& responseBody, int& status)
{
    ++requests_;

    std::shared_ptr<BidRequest> request;
    try {
        OpenRtbOptions options;
        options.exchange = exchangeName();
        options.provider = exchangeName();
        request = std::make_shared<BidRequest>(parseOpenRtbBidRequestText(body, options));
    }
    catch (const std::exception& e) {
        ++badRequests_;
        status = 400;
        responseBody = errorJson(e.what()).dump();
        return;
    }
    request->timeAvailableMs = std::clamp(request->timeAvailableMs, 1.0, maxTmaxMs_);

    auto waiter = std::make_shared<Waiter>();
    auto auction = Auction::create(request, [waiter](std::shared_ptr<Auction>) {
        {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            waiter->done = true;
        }
        waiter->cv.notify_all();
    });

    try {
        submit(auction);
    }
    catch (const std::exception& e) {
        ++errors_;
        auction->finish();
        status = 500;
        responseBody = errorJson(e.what()).dump();
        return;
    }

    auto deadline = auction->expiry()
        + std::chrono::duration_cast<Auction::Clock::duration>(
              std::chrono::duration<double, std::milli>(graceMs_));
    bool finished;
    {
        std::unique_lock<std::mutex> lock(waiter->mutex);
        finished = waiter->cv.wait_until(lock, deadline, [&] { return waiter->done; });
    }
    if (!finished) {
        // The router missed the deadline: close the auction so that late bids
        // are refused, and answer no bid.
        ++timeouts_;
        auction->finish();
    }
    else {
        responseBody = buildBidResponse(*auction);
    }

    if (responseBody.empty()) {
        ++noBids_;
        status = 204;
    }
    else {
        ++bids_;
        status = 200;
    }
}

std::string OpenRtbExchangeConnector::buildBidResponse(const Auction& auction) const
{
    const BidRequest& request = auction.request();
    const std::string exchange = exchangeName();

    json bids = json::array();
    std::string currency;

    for (size_t spot = 0; spot < auction.numSpots(); ++spot) {
        auto best = auction.bestResponse(spot);
        if (!best || !best->valid() || best->price <= Amount()) continue;
        if (size_t(best->spotIndex) >= request.imp.size()) continue;

        const std::string& impId = request.imp[best->spotIndex].id.toString();

        json bid;
        bid["id"] = auction.id().toString() + "-" + impId;
        bid["impid"] = impId;
        bid["price"] = best->price.toCpm();
        bid["nurl"] = winNoticeUrl_ + "?auctionId=" + urlEncode(auction.id().toString())
            + "&adSpotId=" + urlEncode(impId) + "&price=${AUCTION_PRICE}";
        if (!best->agent.empty()) bid["cid"] = best->agent;

        std::string crid = std::to_string(best->creativeIndex);
        if (best->config && size_t(best->creativeIndex) < best->config->creatives.size()) {
            const Creative& creative = best->config->creatives[best->creativeIndex];
            if (creative.id >= 0) crid = std::to_string(creative.id);
            if (creative.format.valid()) {
                bid["w"] = creative.format.width;
                bid["h"] = creative.format.height;
            }

            std::optional<CreativeData> data;
            auto it = creative.providerData.find(exchange);
            if (it != creative.providerData.end() && it->second)
                data = *static_cast<const CreativeData*>(it->second.get());
            else
                data = readCreativeData(creative, exchange);
            if (data) {
                if (!data->crid.empty()) crid = data->crid;
                if (!data->adm.empty()) bid["adm"] = data->adm;
                if (!data->adomain.empty()) bid["adomain"] = data->adomain;
            }
        }
        bid["crid"] = crid;

        if (currency.empty()) currency = toString(best->price.currencyCode);
        bids.push_back(std::move(bid));
    }

    if (bids.empty()) return {};

    json response;
    response["id"] = auction.id().toString();
    response["cur"] = currency;
    response["seatbid"] = json::array({json{{"bid", std::move(bids)}}});
    return response.dump();
}

} // namespace RTBKIT

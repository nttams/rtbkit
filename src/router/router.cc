#include "router/router.h"

#include "agent/protocol.h"
#include "agent/zmq_util.h"
#include "filters/filter_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace RTBKIT {

using nlohmann::json;
using Clock = Auction::Clock;
using TimePoint = Auction::TimePoint;

/** Budgets and floors are in this currency (see docs/ARCHITECTURE.md, Units). */
constexpr CurrencyCode kBidCurrency = CurrencyCode::USD;

RouterConfig RouterConfig::fromJson(const json& j)
{
    RouterConfig config;
    try {
        if (j.contains("agentEndpoint"))
            config.agentEndpoint = j.at("agentEndpoint").get<std::string>();
        if (j.contains("pingIntervalMs"))
            config.pingIntervalMs = j.at("pingIntervalMs").get<int>();
        if (j.contains("agentTimeoutMs"))
            config.agentTimeoutMs = j.at("agentTimeoutMs").get<int>();
        if (j.contains("lossTimeoutMs"))
            config.lossTimeoutMs = j.at("lossTimeoutMs").get<int>();
    }
    catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid router config: ") + e.what());
    }
    return config;
}

json RouterConfig::toJson() const
{
    return json{{"agentEndpoint", agentEndpoint},
                {"pingIntervalMs", pingIntervalMs},
                {"agentTimeoutMs", agentTimeoutMs},
                {"lossTimeoutMs", lossTimeoutMs}};
}


/******************************************************************************/
/* IMPL                                                                       */
/******************************************************************************/

struct Router::Impl {
    struct Stats {
        std::atomic<uint64_t> auctions{0}, auctionsSent{0}, noAgents{0}, bids{0},
            invalid{0}, nobudget{0}, toolate{0}, expired{0}, outbid{0}, wins{0},
            losses{0}, campaignEvents{0}, agents{0}, auctionsInFlight{0}, errors{0};
    };

    struct AgentState {
        std::string name;
        std::string identity;   ///< zmq routing id
        std::shared_ptr<AgentConfig> config;
        TimePoint lastSeen;
        TimePoint lastPing;
        int inFlight = 0;
    };

    struct Participant {
        std::vector<protocol::AuctionSpot> spots;
        bool answered = false;
    };

    struct Accepted {
        Response response;
        std::string itemId;
        bool reserved = false;   ///< banker authorized it
    };

    struct AuctionState {
        std::shared_ptr<Auction> auction;
        std::unordered_map<std::string, Participant> participants;
        std::vector<Accepted> accepted;
        size_t pending = 0;      ///< participants that have not answered
    };

    Impl(RouterConfig cfg, std::shared_ptr<Banker> bank)
        : config(std::move(cfg)), banker(std::move(bank)), mailbox(context),
          rng(std::random_device{}())
    {
        if (!banker) throw std::invalid_argument("router needs a banker");
        pool.initWithDefaultFilters();
    }

    // Shared, set up before start().
    RouterConfig config;
    std::shared_ptr<Banker> banker;
    std::vector<std::shared_ptr<ExchangeConnector>> exchanges;
    SubmittedAuctionSink sink;

    zmq::context_t context;
    zmqutil::Mailbox mailbox;
    std::unique_ptr<zmq::socket_t> socket;
    std::string endpoint;
    std::jthread thread;
    std::atomic<bool> stopped{false};
    Stats stats;

    // Loop thread only.
    FilterPool pool;
    std::unordered_map<std::string, AgentState> agents;          // by name
    std::unordered_map<std::string, std::string> identityToName;
    std::unordered_map<std::string, AuctionState> auctions;      // by auction id
    std::multimap<TimePoint, std::string> expiries;
    std::mt19937 rng;
    bool stopping = false;
    bool logged = false;            // error log rate limit, see logError
    TimePoint lastErrorLog;
    uint64_t suppressedLogs = 0;

    /** Counts and logs an exception that was caught instead of killing the loop. */
    void noteError(const char* where, const std::exception& e)
    {
        logError(where, e.what());
    }

    void noteError(const char* where) { logError(where, "unknown exception"); }

    /** Every error is counted; the log is rate limited (one line per second,
        with the number of lines skipped) so a failing socket cannot flood it. */
    void logError(const char* where, const char* what)
    {
        ++stats.errors;
        const auto now = Clock::now();
        if (logged && now - lastErrorLog < std::chrono::seconds(1)) {
            ++suppressedLogs;
            return;
        }
        std::cerr << "router: " << where << ": " << what;
        if (suppressedLogs) std::cerr << " (" << suppressedLogs << " similar errors not shown)";
        std::cerr << std::endl;
        logged = true;
        lastErrorLog = now;
        suppressedLogs = 0;
    }

    /** Runs `body`; an exception is logged and counted, never propagated. */
    template<typename F>
    void guarded(const char* where, F&& body)
    {
        try {
            body();
        }
        catch (const std::exception& e) {
            noteError(where, e);
        }
        catch (...) {
            noteError(where);
        }
    }

    /** Never throws; does nothing once the socket is closed. */
    void send(const std::string& identity, const protocol::Message& message)
    {
        if (!socket) return;
        guarded("send", [&] {
            auto frames = protocol::encode(message);
            frames.insert(frames.begin(), identity);
            zmqutil::sendFrames(*socket, frames);
        });
    }

    /** auction->finish() runs the exchange's callback, which may throw. */
    void finishQuietly(Auction& auction)
    {
        guarded("auction finish", [&] { auction.finish(); });
    }

    void sendError(const std::string& identity, const std::string& reason,
                   const Id& auctionId = Id())
    {
        send(identity, protocol::ErrorMsg{reason, auctionId});
    }

    const ExchangeConnector* findExchange(const std::string& name) const
    {
        for (const auto& exchange : exchanges)
            if (exchange->exchangeName() == name) return exchange.get();
        if (name.empty() && !exchanges.empty()) return exchanges.front().get();
        return nullptr;
    }

    /************************ event loop ************************/

    void run(std::stop_token token)
    {
        auto housekeepingInterval = std::chrono::milliseconds(std::max(
            1, std::min(config.pingIntervalMs, config.agentTimeoutMs) / 4));
        housekeepingInterval = std::min(housekeepingInterval, std::chrono::milliseconds(50));
        auto nextHousekeeping = Clock::now() + housekeepingInterval;

        zmq::pollitem_t items[] = {
            {socket->handle(), 0, ZMQ_POLLIN, 0},
            {mailbox.socket().handle(), 0, ZMQ_POLLIN, 0},
        };

        bool terminated = false;
        while (!token.stop_requested()) {
            auto now = Clock::now();
            auto wakeAt = nextHousekeeping;
            if (!expiries.empty()) wakeAt = std::min(wakeAt, expiries.begin()->first);
            auto wait = std::chrono::ceil<std::chrono::milliseconds>(wakeAt - now);
            if (wait < std::chrono::milliseconds(0)) wait = std::chrono::milliseconds(0);

            try {
                zmq::poll(items, 2, wait);
            }
            catch (const zmq::error_t& e) {
                if (e.num() == ETERM) break;   // the context is gone: nothing left to serve
                noteError("poll", e);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                items[0].revents = items[1].revents = 0;
            }
            catch (const std::exception& e) {
                noteError("poll", e);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                items[0].revents = items[1].revents = 0;
            }

            if (items[1].revents & ZMQ_POLLIN) mailbox.drain();
            if (items[0].revents & ZMQ_POLLIN) {
                std::vector<std::string> frames;
                for (int i = 0; i < 256; ++i) {
                    bool got = false;
                    try {
                        got = zmqutil::recvFrames(*socket, frames);
                    }
                    catch (const zmq::error_t& e) {
                        if (e.num() == ETERM) { terminated = true; break; }
                        noteError("receive", e);
                        // Back off: a socket that keeps failing must not spin the loop.
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        break;
                    }
                    catch (const std::exception& e) {
                        noteError("receive", e);
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        break;
                    }
                    if (!got) break;
                    handleFrames(frames);
                }
                if (terminated) break;
            }

            now = Clock::now();
            expireAuctions(now);
            if (now >= nextHousekeeping) {
                guarded("housekeeping", [&] { housekeeping(now); });
                nextHousekeeping = now + housekeepingInterval;
            }
        }

        // Shutting down: nobody is left waiting for an auction.
        stopping = true;
        mailbox.drain();
        std::vector<std::string> keys;
        for (const auto& entry : auctions) keys.push_back(entry.first);
        for (const auto& key : keys) finishAuction(key);

        // Nothing may use the agent tables once the loop is gone.
        agents.clear();
        identityToName.clear();
        expiries.clear();
        stats.agents = 0;
    }

    /************************ agent messages ************************/

    void handleFrames(const std::vector<std::string>& frames)
    {
        guarded("handleFrames", [&] { handleFramesImpl(frames); });
    }

    void handleFramesImpl(const std::vector<std::string>& frames)
    {
        if (frames.size() < 2) return;
        const std::string& identity = frames[0];
        protocol::Message message;
        try {
            message = protocol::decode(protocol::Frames(frames.begin() + 1, frames.end()));
        }
        catch (const std::exception& e) {
            sendError(identity, std::string("bad message: ") + e.what());
            return;
        }

        if (auto* configMsg = std::get_if<protocol::ConfigMsg>(&message)) {
            handleConfig(identity, std::move(*configMsg));
            return;
        }

        auto known = identityToName.find(identity);
        if (known == identityToName.end()) {
            send(identity, protocol::NeedConfigMsg{});
            return;
        }
        agents.at(known->second).lastSeen = Clock::now();

        if (auto* bid = std::get_if<protocol::BidMsg>(&message))
            handleBid(identity, known->second, *bid);
        else if (auto* ping = std::get_if<protocol::PingMsg>(&message))
            send(identity, protocol::PongMsg{ping->token});
        else if (!std::holds_alternative<protocol::PongMsg>(message))
            sendError(identity, "unexpected message " + protocol::toString(protocol::typeOf(message)));
    }

    void handleConfig(const std::string& identity, protocol::ConfigMsg msg)
    {
        if (msg.agent.empty()) {
            sendError(identity, "config without agent name");
            return;
        }
        auto config = std::make_shared<AgentConfig>(std::move(msg.config));
        config->name = msg.agent;

        try {
            bool compatible = exchanges.empty();
            for (const auto& exchange : exchanges)
                if (exchange->prepareConfig(*config)) compatible = true;
            if (!compatible) {
                sendError(identity, "config is not compatible with any exchange");
                return;
            }

            pool.addConfig(msg.agent, config);
        }
        catch (const std::exception& e) {
            sendError(identity, std::string("config rejected: ") + e.what());
            return;
        }

        auto now = Clock::now();
        auto [it, added] = agents.try_emplace(msg.agent);
        AgentState& agent = it->second;
        if (!added && agent.identity != identity) identityToName.erase(agent.identity);
        agent.name = msg.agent;
        agent.identity = identity;
        agent.config = std::move(config);
        agent.lastSeen = now;
        agent.lastPing = now;
        identityToName[identity] = msg.agent;
        stats.agents = agents.size();

        send(identity, protocol::GotConfigMsg{msg.agent});
    }

    void removeAgent(const std::string& name)
    {
        auto it = agents.find(name);
        if (it == agents.end()) return;
        identityToName.erase(it->second.identity);
        agents.erase(it);
        pool.removeConfig(name);
        stats.agents = agents.size();

        // Auctions do not wait for an agent that is gone.
        std::vector<std::string> done;
        for (auto& [key, state] : auctions) {
            auto p = state.participants.find(name);
            if (p == state.participants.end() || p->second.answered) continue;
            p->second.answered = true;
            if (--state.pending == 0) done.push_back(key);
        }
        for (const auto& key : done) finishAuction(key);
    }

    void housekeeping(TimePoint now)
    {
        const auto pingInterval = std::chrono::milliseconds(config.pingIntervalMs);
        const auto timeout = std::chrono::milliseconds(config.agentTimeoutMs);
        std::vector<std::string> dead;
        for (auto& [name, agent] : agents) {
            const auto idle = now - agent.lastSeen;
            if (idle >= timeout) {
                dead.push_back(name);
            }
            else if (idle >= pingInterval && now - agent.lastPing >= pingInterval) {
                send(agent.identity, protocol::PingMsg{"router"});
                agent.lastPing = now;
            }
        }
        for (const auto& name : dead) removeAgent(name);
    }

    /************************ auctions ************************/

    /** Posted to the loop: whatever goes wrong, the auction ends. */
    void handleAuction(std::shared_ptr<Auction> auction)
    {
        guarded("handleAuction", [&] { handleAuctionImpl(auction); });
        // finish() is idempotent: only an auction the router now owns stays open.
        auto owned = auctions.find(auction->id().toString());
        if (owned == auctions.end() || owned->second.auction != auction)
            finishQuietly(*auction);
    }

    void handleAuctionImpl(const std::shared_ptr<Auction>& auction)
    {
        ++stats.auctions;
        if (auction->finished()) return;

        const std::string key = auction->id().toString();
        const auto now = Clock::now();
        if (stopping || auction->expired(now) || auctions.contains(key)) {
            if (auction->expired(now)) ++stats.expired;
            auction->finish();
            return;
        }

        const ExchangeConnector* exchange = findExchange(auction->request().exchange);
        if (!exchange) {
            auction->finish();
            return;
        }

        // The filters need a request date; connectors that left it unset get "now".
        const BidRequest* request = &auction->request();
        BidRequest dated;
        if (request->timestamp == Date()) {
            dated = *request;
            dated.timestamp = std::chrono::system_clock::now();
            request = &dated;
        }

        FilterPool::Result result;
        try {
            result = pool.filter(*request, exchange);
        }
        catch (const std::exception&) {
            auction->finish();
            return;
        }

        AuctionState state;
        state.auction = auction;
        for (const auto& match : result.matched) {
            auto it = agents.find(match.config);
            if (it == agents.end()) continue;
            AgentState& agent = it->second;
            const AgentConfig& config = *agent.config;

            if (agent.inFlight >= config.maxInFlight) continue;
            if (auction->timeLeftMs(now) < config.minTimeAvailableMs) continue;
            if (config.bidProbability < 1.0f
                && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) >= config.bidProbability)
                continue;

            Participant participant;
            for (const auto& spot : match.spots)
                participant.spots.push_back(protocol::AuctionSpot{
                    int(spot.spot), std::vector<int>(spot.creatives.begin(), spot.creatives.end())});
            if (participant.spots.empty()) continue;

            send(agent.identity, protocol::AuctionMsg{
                auction->id(), auction->timeLeftMs(now), auction->requestPtr(),
                participant.spots});
            ++agent.inFlight;
            state.participants.emplace(agent.name, std::move(participant));
        }

        if (state.participants.empty()) {
            ++stats.noAgents;
            auction->finish();
            return;
        }

        state.pending = state.participants.size();
        ++stats.auctionsSent;
        expiries.emplace(auction->expiry(), key);
        auctions.emplace(key, std::move(state));
        stats.auctionsInFlight = auctions.size();
    }

    /** Checks one bid against what the agent was offered. Empty = valid. */
    static std::string checkBid(const Bid& bid, const AgentConfig& config,
                                const Participant& participant)
    {
        if (!bid.valid()) return "bid needs spot, creative and a positive price";
        if (bid.price.currencyCode != kBidCurrency)
            return "bid currency " + bid.price.currencyStr() + " is not "
                   + Amount(kBidCurrency, 0).currencyStr();
        if (!bid.account.empty() && !bid.account.hasPrefix(config.account))
            return "account " + bid.account.toString() + " is not under "
                   + config.account.toString();
        for (const auto& spot : participant.spots) {
            if (spot.spotIndex != bid.spotIndex) continue;
            if (std::find(spot.creatives.begin(), spot.creatives.end(),
                          bid.creativeIndex) != spot.creatives.end())
                return "";
            return "creative " + std::to_string(bid.creativeIndex)
                   + " is not eligible for spot " + std::to_string(bid.spotIndex);
        }
        return "spot " + std::to_string(bid.spotIndex) + " is not biddable";
    }

    void handleBid(const std::string& identity, const std::string& name,
                   const protocol::BidMsg& msg)
    {
        const std::string key = msg.auctionId.toString();
        auto auctionIt = auctions.find(key);
        if (auctionIt == auctions.end()) {
            ++stats.toolate;
            send(identity, protocol::TooLateMsg{msg.auctionId});
            return;
        }
        AuctionState& state = auctionIt->second;
        auto participantIt = state.participants.find(name);
        if (participantIt == state.participants.end()) {
            ++stats.invalid;
            send(identity, protocol::InvalidMsg{msg.auctionId, "auction was not sent to this agent"});
            return;
        }
        Participant& participant = participantIt->second;
        if (participant.answered) {
            ++stats.invalid;
            send(identity, protocol::InvalidMsg{msg.auctionId, "auction already answered"});
            return;
        }
        if (state.auction->expired()) {
            ++stats.toolate;
            send(identity, protocol::TooLateMsg{msg.auctionId});
            return;
        }

        participant.answered = true;
        AgentState& agent = agents.at(name);
        const AgentConfig& config = *agent.config;

        // Validate the whole message first: one bad bid rejects it all.
        std::string reason;
        std::vector<int> seenSpots;
        for (const Bid& bid : msg.bids) {
            reason = checkBid(bid, config, participant);
            if (reason.empty() && std::find(seenSpots.begin(), seenSpots.end(),
                                            bid.spotIndex) != seenSpots.end())
                reason = "two bids for spot " + std::to_string(bid.spotIndex);
            if (!reason.empty()) break;
            seenSpots.push_back(bid.spotIndex);
        }

        if (!reason.empty()) {
            ++stats.invalid;
            send(identity, protocol::InvalidMsg{msg.auctionId, reason});
        }
        else {
            bool refused = false;
            for (const Bid& bid : msg.bids) {
                bool accepted = false;
                guarded("acceptBid", [&] { accepted = acceptBid(state, agent, bid); });
                if (!accepted) refused = true;
            }
            if (refused) send(identity, protocol::NoBudgetMsg{msg.auctionId});
        }

        if (--state.pending == 0) finishAuction(key);
    }

    /** Banker check and recording of one valid bid. False if the banker refused. */
    bool acceptBid(AuctionState& state, const AgentState& agent, const Bid& bid)
    {
        const AgentConfig& config = *agent.config;
        Response response;
        response.agent = agent.name;
        response.account = bid.account.empty() ? config.account : bid.account;
        response.price = bid.price;
        response.spotIndex = bid.spotIndex;
        response.creativeIndex = bid.creativeIndex;
        response.priority = bid.priority;
        response.test = config.test;
        response.bidData = bid.bidData;
        response.meta = bid.meta;
        response.config = agent.config;

        const Auction& auction = *state.auction;
        const std::string itemId = bidItemId(
            auction.id(), auction.request().imp.at(size_t(bid.spotIndex)).id,
            response.agent);

        // Test agents never spend money.
        bool reserved = false;
        if (!config.test) {
            // A banker that fails (connection lost, bad currency) cannot vouch
            // for the money: same as a refusal.
            bool authorized = false;
            guarded("authorizeBid", [&] {
                authorized = banker->authorizeBid(response.account, itemId, response.price);
            });
            if (!authorized) {
                ++stats.nobudget;
                return false;
            }
            reserved = true;
        }

        // From here on the money is reserved: whatever throws, release it.
        try {
            auto result = state.auction->setResponse(response);
            if (result == Auction::SetResult::Best || result == Auction::SetResult::Outbid) {
                state.accepted.push_back({response, itemId, reserved});
                ++stats.bids;
                return true;
            }
        }
        catch (...) {
            if (reserved)
                guarded("cancelBid", [&] { banker->cancelBid(response.account, itemId); });
            throw;
        }

        // The auction refused it (bad currency, finished): give the money back.
        if (reserved)
            guarded("cancelBid", [&] { banker->cancelBid(response.account, itemId); });
        ++stats.invalid;
        send(agent.identity, protocol::InvalidMsg{auction.id(), "bid rejected by auction"});
        return true;
    }

    void expireAuctions(TimePoint now)
    {
        guarded("expireAuctions", [&] { expireAuctionsImpl(now); });
    }

    void expireAuctionsImpl(TimePoint now)
    {
        while (!expiries.empty() && expiries.begin()->first <= now) {
            const std::string key = expiries.begin()->second;
            expiries.erase(expiries.begin());
            auto it = auctions.find(key);
            if (it == auctions.end() || it->second.auction->expiry() > now) continue;
            ++stats.expired;
            finishAuction(key);
        }
    }

    /** Always ends with auction->finish(): the exchange is waiting for it. */
    void finishAuction(const std::string& key)
    {
        auto it = auctions.find(key);
        if (it == auctions.end()) return;
        AuctionState state = std::move(it->second);
        auctions.erase(it);
        stats.auctionsInFlight = auctions.size();

        guarded("finishAuction", [&] { settleAuction(state); });

        for (const auto& [name, participant] : state.participants) {
            auto agentIt = agents.find(name);
            if (agentIt != agents.end() && agentIt->second.inFlight > 0)
                --agentIt->second.inFlight;
        }

        finishQuietly(*state.auction);
    }

    /** Banker cancels for the losers, loss messages, submission of the winners.
        Each banker call and the sink are guarded on their own, so one failure
        does not skip the rest. */
    void settleAuction(AuctionState& state)
    {
        Auction& auction = *state.auction;
        const auto systemNow = std::chrono::system_clock::now();

        std::vector<std::optional<Response>> best;
        for (size_t spot = 0; spot < auction.numSpots(); ++spot)
            best.push_back(auction.bestResponse(spot));

        for (const Accepted& accepted : state.accepted) {
            const Response& response = accepted.response;
            const auto& winner = best.at(size_t(response.spotIndex));
            if (winner && winner->agent == response.agent) continue;

            if (accepted.reserved)
                guarded("cancelBid", [&] { banker->cancelBid(response.account, accepted.itemId); });

            ++stats.outbid;
            auto agentIt = agents.find(response.agent);
            if (agentIt != agents.end() && agentIt->second.config->lossFormat != BRF_NONE) {
                MatchedWinLoss loss;
                loss.type = MatchedWinLoss::Type::LOSS;
                loss.auctionId = auction.id();
                loss.adSpotId = auction.request().imp.at(size_t(response.spotIndex)).id;
                loss.timestamp = systemNow;
                loss.bid = response;
                send(agentIt->second.identity, protocol::LossMsg{loss});
            }
        }

        if (!sink) return;
        for (size_t spot = 0; spot < best.size(); ++spot) {
            if (!best[spot]) continue;
            guarded("submitted auction sink", [&] {
                SubmittedAuctionEvent event;
                event.auctionId = auction.id();
                event.adSpotId = auction.request().imp[spot].id;
                event.lossTimeout = systemNow + std::chrono::milliseconds(config.lossTimeoutMs);
                event.bid = *best[spot];
                event.request = auction.requestPtr();
                sink(event);
            });
        }
    }

    /************************ events from post auction ************************/

    void forwardWinLoss(const MatchedWinLoss& event)
    {
        guarded("forwardWinLoss", [&] { forwardWinLossImpl(event); });
    }

    void forwardWinLossImpl(const MatchedWinLoss& event)
    {
        if (stopped) return;
        auto it = agents.find(event.bid.agent);
        if (it == agents.end()) return;
        const AgentState& agent = it->second;
        if (event.type == MatchedWinLoss::Type::LOSS) {
            ++stats.losses;
            if (agent.config->lossFormat != BRF_NONE)
                send(agent.identity, protocol::LossMsg{event});
        }
        else {
            ++stats.wins;
            if (agent.config->winFormat != BRF_NONE)
                send(agent.identity, protocol::WinMsg{event});
        }
    }

    void forwardCampaignEvent(const MatchedCampaignEvent& event)
    {
        guarded("forwardCampaignEvent", [&] { forwardCampaignEventImpl(event); });
    }

    void forwardCampaignEventImpl(const MatchedCampaignEvent& event)
    {
        if (stopped) return;
        auto it = agents.find(event.bid.agent);
        if (it == agents.end()) return;
        ++stats.campaignEvents;
        send(it->second.identity, protocol::CampaignEventMsg{event});
    }
};


/******************************************************************************/
/* ROUTER                                                                     */
/******************************************************************************/

Router::Router(RouterConfig config, std::shared_ptr<Banker> banker)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(banker)))
{
}

Router::~Router()
{
    stop();
}

void Router::addExchange(std::shared_ptr<ExchangeConnector> connector)
{
    connector->setAuctionHandler(
        [this](std::shared_ptr<Auction> auction) { injectAuction(std::move(auction)); });
    impl_->exchanges.push_back(std::move(connector));
}

void Router::setSubmittedAuctionSink(SubmittedAuctionSink sink)
{
    impl_->sink = std::move(sink);
}

void Router::start()
{
    if (impl_->thread.joinable()) return;
    impl_->socket = std::make_unique<zmq::socket_t>(impl_->context, zmq::socket_type::router);
    impl_->socket->set(zmq::sockopt::linger, 0);
    // A restarted agent with the same identity takes over its old connection.
    impl_->socket->set(zmq::sockopt::router_handover, 1);
    impl_->socket->bind(impl_->config.agentEndpoint);
    impl_->endpoint = impl_->socket->get(zmq::sockopt::last_endpoint);
    impl_->thread = std::jthread([impl = impl_.get()](std::stop_token token) {
        impl->run(token);
    });
}

void Router::stop()
{
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->mailbox.post([] {});
        impl_->thread.join();
    }
    impl_->stopped = true;
    impl_->stopping = true;
    impl_->mailbox.drain();   // finishes auctions queued before start() or during stop
    if (impl_->socket) {
        impl_->socket->close();
        impl_->socket.reset();
    }
}

void Router::injectAuction(std::shared_ptr<Auction> auction)
{
    if (impl_->stopped) {
        auction->finish();
        return;
    }
    impl_->mailbox.post([impl = impl_.get(), auction = std::move(auction)]() mutable {
        impl->handleAuction(std::move(auction));
    });
}

void Router::onMatchedWinLoss(const MatchedWinLoss& event)
{
    if (impl_->stopped) return;
    impl_->mailbox.post([impl = impl_.get(), event] { impl->forwardWinLoss(event); });
}

void Router::onMatchedCampaignEvent(const MatchedCampaignEvent& event)
{
    if (impl_->stopped) return;
    impl_->mailbox.post([impl = impl_.get(), event] { impl->forwardCampaignEvent(event); });
}

std::string Router::boundEndpoint() const
{
    return impl_->endpoint;
}

json Router::stats() const
{
    const auto& s = impl_->stats;
    return json{{"auctions", s.auctions.load()},
                {"auctionsSent", s.auctionsSent.load()},
                {"noAgents", s.noAgents.load()},
                {"bids", s.bids.load()},
                {"invalid", s.invalid.load()},
                {"nobudget", s.nobudget.load()},
                {"toolate", s.toolate.load()},
                {"expired", s.expired.load()},
                {"outbid", s.outbid.load()},
                {"wins", s.wins.load()},
                {"losses", s.losses.load()},
                {"campaignEvents", s.campaignEvents.load()},
                {"agents", s.agents.load()},
                {"auctionsInFlight", s.auctionsInFlight.load()},
                {"errors", s.errors.load()}};
}

} // namespace RTBKIT

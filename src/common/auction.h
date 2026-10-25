// Simplified port of rtbkit/common/auction.h, bids.h and win_cost_model.h.
//
// Life of an auction:
//   exchange connector: Auction::create(request, onFinished)  -> router
//   router:             auction->setResponse(response) per agent bid
//   router (deadline or all agents answered): auction->finish()
//   -> the completion callback runs once; the connector writes the HTTP reply
//      from auction->bestResponse(spot).
//
// Prices are Amounts per impression in micro-units: USD_CPM(1.5) == 1500
// micro USD. Everything (bids, floors, banker, win prices) uses that unit.
#pragma once

#include "common/account_key.h"
#include "common/bid_request.h"
#include "common/currency.h"
#include "common/id.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace RTBKIT {

struct AgentConfig;


/******************************************************************************/
/* BID / BIDS                                                                 */
/******************************************************************************/

/** One bid made by an agent: "show creative `creativeIndex` of my config in
    spot `spotIndex` for at most `price`". */
struct Bid {
    int spotIndex = -1;       ///< index into BidRequest::imp
    int creativeIndex = -1;   ///< index into AgentConfig::creatives
    Amount price;             ///< max price per impression
    int priority = 0;         ///< higher wins before price is compared

    /** Account to bill. Empty means the agent config's account. */
    AccountKey account;

    std::string bidData;      ///< opaque, echoed back in WIN / LOSS
    nlohmann::json meta;      ///< opaque, echoed back in WIN / LOSS

    /** Indexes are non-negative and the price is positive. */
    bool valid() const
    {
        return spotIndex >= 0 && creativeIndex >= 0 && price > Amount();
    }

    nlohmann::json toJson() const;
    /** Throws std::runtime_error. */
    static Bid fromJson(const nlohmann::json& j);
};

/** All the bids an agent makes for one auction. JSON form is
    {"bids": [ {spot, creative, price, ...}, ... ], "meta": ...}; parsing also
    accepts a bare array of bids. */
struct Bids : public std::vector<Bid> {
    nlohmann::json meta;

    nlohmann::json toJson() const;
    /** Throws std::runtime_error. */
    static Bids fromJson(const nlohmann::json& j);
};


/******************************************************************************/
/* RESPONSE                                                                   */
/******************************************************************************/

/** A bid as the router sees it: a Bid plus who made it. */
struct Response {
    std::string agent;          ///< agent name from its config
    AccountKey account;         ///< account that pays
    Amount price;               ///< max price per impression
    int spotIndex = -1;
    int creativeIndex = -1;
    int priority = 0;
    bool test = false;          ///< from AgentConfig::test, never charged
    std::string bidData;
    nlohmann::json meta;

    /** Set by the router so that the exchange connector can find the
        creative (config->creatives[creativeIndex]) and providerData. Not
        part of the JSON form. */
    std::shared_ptr<const AgentConfig> config;

    bool valid() const { return spotIndex >= 0 && creativeIndex >= 0; }

    nlohmann::json toJson() const;
    /** Throws std::runtime_error. config is left empty. */
    static Response fromJson(const nlohmann::json& j);
};


/******************************************************************************/
/* AUCTION                                                                    */
/******************************************************************************/

class Auction : public std::enable_shared_from_this<Auction> {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Completion = std::function<void(std::shared_ptr<Auction>)>;

    enum class SetResult {
        Best,         ///< is now the best response of its spot
        Outbid,       ///< accepted, but another response is better
        InvalidSpot,  ///< spot index out of range
        BadPrice,     ///< not positive, or currency does not match the others
        Finished      ///< auction already finished, ignored
    };

    /** The only way to build an auction (it must be owned by a shared_ptr).
        Expiry is start + request->timeAvailableMs. */
    static std::shared_ptr<Auction> create(
            std::shared_ptr<const BidRequest> request,
            Completion onFinished = {},
            TimePoint start = Clock::now());

    /** Same with an explicit expiry. */
    static std::shared_ptr<Auction> createWithExpiry(
            std::shared_ptr<const BidRequest> request,
            TimePoint start, TimePoint expiry, Completion onFinished = {});

    const Id& id() const { return request_->auctionId; }
    const BidRequest& request() const { return *request_; }
    std::shared_ptr<const BidRequest> requestPtr() const { return request_; }

    TimePoint start() const { return start_; }
    TimePoint expiry() const { return expiry_; }
    bool expired(TimePoint now = Clock::now()) const { return now >= expiry_; }
    double timeLeftMs(TimePoint now = Clock::now()) const;

    size_t numSpots() const { return request_->imp.size(); }

    /** Records a response for response.spotIndex. Best means higher
        priority, then higher price; the first one wins a tie. Thread safe. */
    SetResult setResponse(const Response& response);

    /** Best response of the spot, if any. */
    std::optional<Response> bestResponse(size_t spot) const;

    /** Highest price among the responses that are not the best one; zero if
        there are fewer than two. Used by second price. */
    Amount secondPrice(size_t spot) const;

    size_t numResponses(size_t spot) const;
    bool hasResponses() const;

    /** Replaces the completion callback. Call before the auction is shared. */
    void setCompletion(Completion onFinished);

    /** Marks the auction finished and runs the completion callback, once, on
        the calling thread. Returns false if it was already finished. */
    bool finish();
    bool finished() const;

private:
    Auction(std::shared_ptr<const BidRequest> request, TimePoint start,
            TimePoint expiry, Completion onFinished);

    struct Spot {
        std::optional<Response> best;
        Amount second;
        size_t count = 0;
    };

    std::shared_ptr<const BidRequest> request_;
    TimePoint start_;
    TimePoint expiry_;

    mutable std::mutex mutex_;
    std::vector<Spot> spots_;
    Completion onFinished_;
    bool finished_ = false;
};


/******************************************************************************/
/* WIN COST MODEL                                                             */
/******************************************************************************/

/** What the winner pays. */
struct WinCostModel {
    enum class Type {
        FirstPrice,   ///< pay the bid
        SecondPrice   ///< pay max(second best bid, reserve), never above the bid
    };

    Type type = Type::SecondPrice;

    /** FirstPrice or SecondPrice according to the request. */
    static WinCostModel forRequest(const BidRequest& request);

    Amount evaluate(const Amount& bid, const Amount& secondPrice,
                    const Amount& reserve) const;

    /** Cost for the best response of the spot; zero amount if none. */
    Amount evaluate(const Auction& auction, size_t spot) const;

    nlohmann::json toJson() const;
    /** {"type": "firstPrice" | "secondPrice"}. Throws std::runtime_error. */
    static WinCostModel fromJson(const nlohmann::json& j);
};

} // namespace RTBKIT

#include "common/auction.h"

#include <algorithm>

namespace RTBKIT {

using nlohmann::json;

namespace {

[[noreturn]] void bad(const std::string& what)
{
    throw std::runtime_error(what);
}

template<typename T>
T getOr(const json& j, const char* key, T fallback)
{
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? fallback : it->get<T>();
}

Amount amountOf(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end()) bad(std::string("missing ") + key);
    return it->get<Amount>();
}

AccountKey accountOf(const json& j)
{
    auto it = j.find("account");
    return it == j.end() || it->is_null() ? AccountKey() : AccountKey::fromJson(*it);
}

// Runs fn, turning json/value errors into std::runtime_error.
template<typename Fn>
auto guarded(const char* what, Fn&& fn)
{
    try {
        return fn();
    }
    catch (const json::exception& e) {
        bad(std::string("invalid ") + what + ": " + e.what());
    }
    catch (const std::invalid_argument& e) {
        bad(std::string("invalid ") + what + ": " + e.what());
    }
}

} // namespace


/******************************************************************************/
/* BID / BIDS / RESPONSE                                                      */
/******************************************************************************/

json Bid::toJson() const
{
    json j = json::object();
    j["spot"] = spotIndex;
    j["creative"] = creativeIndex;
    j["price"] = price;
    j["priority"] = priority;
    if (!account.empty()) j["account"] = account.toJson();
    if (!bidData.empty()) j["bidData"] = bidData;
    if (!meta.is_null()) j["meta"] = meta;
    return j;
}

Bid Bid::fromJson(const json& j)
{
    if (!j.is_object()) bad("a bid must be an object");
    return guarded("bid", [&] {
        Bid b;
        b.spotIndex = j.at("spot").get<int>();
        b.creativeIndex = j.at("creative").get<int>();
        b.price = amountOf(j, "price");
        b.priority = getOr(j, "priority", 0);
        b.account = accountOf(j);
        b.bidData = getOr<std::string>(j, "bidData", "");
        if (j.contains("meta")) b.meta = j.at("meta");
        return b;
    });
}

json Bids::toJson() const
{
    json list = json::array();
    for (const Bid& b : *this) list.push_back(b.toJson());
    json j = json::object();
    j["bids"] = list;
    if (!meta.is_null()) j["meta"] = meta;
    return j;
}

Bids Bids::fromJson(const json& j)
{
    Bids result;
    const json* list = &j;
    if (j.is_object()) {
        auto it = j.find("bids");
        if (it == j.end()) bad("bids object needs a \"bids\" array");
        list = &*it;
        if (j.contains("meta")) result.meta = j.at("meta");
    }
    if (!list->is_array()) bad("bids must be an array");
    for (const json& b : *list) result.push_back(Bid::fromJson(b));
    return result;
}

json Response::toJson() const
{
    json j = json::object();
    j["agent"] = agent;
    j["account"] = account.toJson();
    j["price"] = price;
    j["spot"] = spotIndex;
    j["creative"] = creativeIndex;
    j["priority"] = priority;
    j["test"] = test;
    if (!bidData.empty()) j["bidData"] = bidData;
    if (!meta.is_null()) j["meta"] = meta;
    return j;
}

Response Response::fromJson(const json& j)
{
    if (!j.is_object()) bad("a response must be an object");
    return guarded("response", [&] {
        Response r;
        r.agent = getOr<std::string>(j, "agent", "");
        r.account = accountOf(j);
        r.price = amountOf(j, "price");
        r.spotIndex = j.at("spot").get<int>();
        r.creativeIndex = j.at("creative").get<int>();
        r.priority = getOr(j, "priority", 0);
        r.test = getOr(j, "test", false);
        r.bidData = getOr<std::string>(j, "bidData", "");
        if (j.contains("meta")) r.meta = j.at("meta");
        return r;
    });
}


/******************************************************************************/
/* AUCTION                                                                    */
/******************************************************************************/

Auction::Auction(std::shared_ptr<const BidRequest> request, TimePoint start,
                 TimePoint expiry, Completion onFinished)
    : request_(std::move(request)), start_(start), expiry_(expiry),
      spots_(request_->imp.size()), onFinished_(std::move(onFinished))
{
}

std::shared_ptr<Auction> Auction::create(
        std::shared_ptr<const BidRequest> request, Completion onFinished,
        TimePoint start)
{
    if (!request) throw std::invalid_argument("auction needs a bid request");
    auto budget = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(request->timeAvailableMs));
    return createWithExpiry(std::move(request), start, start + budget,
                            std::move(onFinished));
}

std::shared_ptr<Auction> Auction::createWithExpiry(
        std::shared_ptr<const BidRequest> request, TimePoint start,
        TimePoint expiry, Completion onFinished)
{
    if (!request) throw std::invalid_argument("auction needs a bid request");
    return std::shared_ptr<Auction>(
            new Auction(std::move(request), start, expiry, std::move(onFinished)));
}

double Auction::timeLeftMs(TimePoint now) const
{
    return std::chrono::duration<double, std::milli>(expiry_ - now).count();
}

Auction::SetResult Auction::setResponse(const Response& response)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (finished_) return SetResult::Finished;
    if (response.spotIndex < 0 || size_t(response.spotIndex) >= spots_.size())
        return SetResult::InvalidSpot;
    if (response.price <= Amount()) return SetResult::BadPrice;

    Spot& spot = spots_[size_t(response.spotIndex)];
    if (spot.best && !spot.best->price.currencyIsCompatible(response.price))
        return SetResult::BadPrice;

    ++spot.count;

    auto raiseSecond = [&](const Amount& price) {
        if (price > spot.second) spot.second = price;
    };

    bool better = !spot.best
        || response.priority > spot.best->priority
        || (response.priority == spot.best->priority
            && response.price > spot.best->price);

    if (!better) {
        raiseSecond(response.price);
        return SetResult::Outbid;
    }

    if (spot.best) raiseSecond(spot.best->price);
    spot.best = response;
    return SetResult::Best;
}

std::optional<Response> Auction::bestResponse(size_t spot) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (spot >= spots_.size()) return std::nullopt;
    return spots_[spot].best;
}

Amount Auction::secondPrice(size_t spot) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return spot < spots_.size() ? spots_[spot].second : Amount();
}

size_t Auction::numResponses(size_t spot) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return spot < spots_.size() ? spots_[spot].count : 0;
}

bool Auction::hasResponses() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return std::any_of(spots_.begin(), spots_.end(),
                       [](const Spot& s) { return s.best.has_value(); });
}

void Auction::setCompletion(Completion onFinished)
{
    std::lock_guard<std::mutex> guard(mutex_);
    onFinished_ = std::move(onFinished);
}

bool Auction::finish()
{
    Completion callback;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (finished_) return false;
        finished_ = true;
        callback = std::move(onFinished_);
        onFinished_ = nullptr;
    }
    if (callback) callback(shared_from_this());
    return true;
}

bool Auction::finished() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return finished_;
}


/******************************************************************************/
/* WIN COST MODEL                                                             */
/******************************************************************************/

WinCostModel WinCostModel::forRequest(const BidRequest& request)
{
    WinCostModel model;
    model.type = request.auctionType == AuctionType::FirstPrice
        ? Type::FirstPrice : Type::SecondPrice;
    return model;
}

Amount WinCostModel::evaluate(const Amount& bid, const Amount& secondPrice,
                              const Amount& reserve) const
{
    if (type == Type::FirstPrice) return bid;
    Amount price = secondPrice;
    if (reserve.currencyIsCompatible(bid) && reserve > price) price = reserve;
    return price.limit(bid);
}

Amount WinCostModel::evaluate(const Auction& auction, size_t spot) const
{
    auto best = auction.bestResponse(spot);
    if (!best) return Amount();
    return evaluate(best->price, auction.secondPrice(spot),
                    auction.request().imp.at(spot).reservePrice);
}

json WinCostModel::toJson() const
{
    return json{{"type", type == Type::FirstPrice ? "firstPrice" : "secondPrice"}};
}

WinCostModel WinCostModel::fromJson(const json& j)
{
    std::string name;
    try {
        name = j.at("type").get<std::string>();
    }
    catch (const json::exception&) {
        bad("win cost model needs a \"type\" string");
    }
    WinCostModel model;
    if (name == "firstPrice") model.type = Type::FirstPrice;
    else if (name == "secondPrice") model.type = Type::SecondPrice;
    else bad("unknown win cost model " + name);
    return model;
}

} // namespace RTBKIT

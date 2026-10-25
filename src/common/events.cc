#include "common/events.h"

namespace RTBKIT {

using nlohmann::json;

namespace {

[[noreturn]] void bad(const std::string& what)
{
    throw std::runtime_error(what);
}

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

Id idOf(const json& j, const char* key) { return Id(j.at(key).get<std::string>()); }

Date dateOf(const json& j, const char* key)
{
    auto it = j.find(key);
    return it == j.end() ? Date() : dateFromMs(it->get<int64_t>());
}

Amount amountOr(const json& j, const char* key)
{
    auto it = j.find(key);
    return it == j.end() ? Amount() : it->get<Amount>();
}

void requireObject(const json& j, const char* what)
{
    if (!j.is_object()) bad(std::string(what) + " must be an object");
}

} // namespace

std::string toString(PostAuctionEventType type)
{
    switch (type) {
    case PostAuctionEventType::WIN: return "WIN";
    case PostAuctionEventType::LOSS: return "LOSS";
    case PostAuctionEventType::CAMPAIGN_EVENT: return "CAMPAIGN_EVENT";
    }
    return "WIN";
}

PostAuctionEventType parsePostAuctionEventType(const std::string& name)
{
    if (name == "WIN") return PostAuctionEventType::WIN;
    if (name == "LOSS") return PostAuctionEventType::LOSS;
    if (name == "CAMPAIGN_EVENT") return PostAuctionEventType::CAMPAIGN_EVENT;
    bad("unknown post auction event type " + name);
}

std::string toString(CampaignEventType type)
{
    return type == CampaignEventType::CLICK ? "CLICK" : "IMPRESSION";
}

CampaignEventType parseCampaignEventType(const std::string& name)
{
    if (name == "IMPRESSION") return CampaignEventType::IMPRESSION;
    if (name == "CLICK") return CampaignEventType::CLICK;
    bad("unknown campaign event " + name);
}

std::string toString(MatchedWinLoss::Type type)
{
    switch (type) {
    case MatchedWinLoss::Type::WIN: return "WIN";
    case MatchedWinLoss::Type::LATE_WIN: return "LATE_WIN";
    case MatchedWinLoss::Type::LOSS: return "LOSS";
    }
    return "LOSS";
}

MatchedWinLoss::Type parseMatchedWinLossType(const std::string& name)
{
    if (name == "WIN") return MatchedWinLoss::Type::WIN;
    if (name == "LATE_WIN") return MatchedWinLoss::Type::LATE_WIN;
    if (name == "LOSS") return MatchedWinLoss::Type::LOSS;
    bad("unknown matched win/loss type " + name);
}


json SubmittedAuctionEvent::toJson() const
{
    json j = json::object();
    j["auctionId"] = auctionId.toString();
    j["adSpotId"] = adSpotId.toString();
    j["lossTimeout"] = dateToMs(lossTimeout);
    j["bid"] = bid.toJson();
    if (request) j["request"] = RTBKIT::toJson(*request);
    return j;
}

SubmittedAuctionEvent SubmittedAuctionEvent::fromJson(const json& j)
{
    requireObject(j, "submitted auction event");
    return guarded("submitted auction event", [&] {
        SubmittedAuctionEvent e;
        e.auctionId = idOf(j, "auctionId");
        e.adSpotId = idOf(j, "adSpotId");
        e.lossTimeout = dateOf(j, "lossTimeout");
        e.bid = Response::fromJson(j.at("bid"));
        if (j.contains("request"))
            e.request = std::make_shared<BidRequest>(bidRequestFromJson(j.at("request")));
        return e;
    });
}

json PostAuctionEvent::toJson() const
{
    json j = json::object();
    j["type"] = RTBKIT::toString(type);
    j["auctionId"] = auctionId.toString();
    j["adSpotId"] = adSpotId.toString();
    j["timestamp"] = dateToMs(timestamp);
    if (type == PostAuctionEventType::WIN) j["winPrice"] = winPrice;
    if (type == PostAuctionEventType::CAMPAIGN_EVENT)
        j["campaignEvent"] = RTBKIT::toString(campaignEvent);
    if (!meta.is_null()) j["meta"] = meta;
    return j;
}

PostAuctionEvent PostAuctionEvent::fromJson(const json& j)
{
    requireObject(j, "post auction event");
    return guarded("post auction event", [&] {
        PostAuctionEvent e;
        e.type = parsePostAuctionEventType(j.at("type").get<std::string>());
        e.auctionId = idOf(j, "auctionId");
        e.adSpotId = idOf(j, "adSpotId");
        e.timestamp = dateOf(j, "timestamp");
        e.winPrice = amountOr(j, "winPrice");
        if (j.contains("campaignEvent"))
            e.campaignEvent = parseCampaignEventType(j.at("campaignEvent").get<std::string>());
        if (j.contains("meta")) e.meta = j.at("meta");
        return e;
    });
}

json MatchedWinLoss::toJson() const
{
    json j = json::object();
    j["type"] = RTBKIT::toString(type);
    j["auctionId"] = auctionId.toString();
    j["adSpotId"] = adSpotId.toString();
    j["timestamp"] = dateToMs(timestamp);
    j["winPrice"] = winPrice;
    j["bid"] = bid.toJson();
    return j;
}

MatchedWinLoss MatchedWinLoss::fromJson(const json& j)
{
    requireObject(j, "matched win/loss");
    return guarded("matched win/loss", [&] {
        MatchedWinLoss m;
        m.type = parseMatchedWinLossType(j.at("type").get<std::string>());
        m.auctionId = idOf(j, "auctionId");
        m.adSpotId = idOf(j, "adSpotId");
        m.timestamp = dateOf(j, "timestamp");
        m.winPrice = amountOr(j, "winPrice");
        m.bid = Response::fromJson(j.at("bid"));
        return m;
    });
}

json MatchedCampaignEvent::toJson() const
{
    json j = json::object();
    j["event"] = RTBKIT::toString(event);
    j["auctionId"] = auctionId.toString();
    j["adSpotId"] = adSpotId.toString();
    j["timestamp"] = dateToMs(timestamp);
    j["bid"] = bid.toJson();
    return j;
}

MatchedCampaignEvent MatchedCampaignEvent::fromJson(const json& j)
{
    requireObject(j, "matched campaign event");
    return guarded("matched campaign event", [&] {
        MatchedCampaignEvent m;
        m.event = parseCampaignEventType(j.at("event").get<std::string>());
        m.auctionId = idOf(j, "auctionId");
        m.adSpotId = idOf(j, "adSpotId");
        m.timestamp = dateOf(j, "timestamp");
        m.bid = Response::fromJson(j.at("bid"));
        return m;
    });
}

} // namespace RTBKIT

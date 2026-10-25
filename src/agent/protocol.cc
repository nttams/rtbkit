#include "agent/protocol.h"

#include "common/bid_request_json.h"

#include <format>

namespace RTBKIT::protocol {

using nlohmann::json;

namespace {

struct TypeName {
    MessageType type;
    const char* name;
};

constexpr TypeName TYPE_NAMES[] = {
    {MessageType::CONFIG, "CONFIG"},
    {MessageType::GOTCONFIG, "GOTCONFIG"},
    {MessageType::NEEDCONFIG, "NEEDCONFIG"},
    {MessageType::AUCTION, "AUCTION"},
    {MessageType::BID, "BID"},
    {MessageType::NOBUDGET, "NOBUDGET"},
    {MessageType::TOOLATE, "TOOLATE"},
    {MessageType::INVALID, "INVALID"},
    {MessageType::ERROR, "ERROR"},
    {MessageType::WIN, "WIN"},
    {MessageType::LOSS, "LOSS"},
    {MessageType::CAMPAIGN_EVENT, "CAMPAIGN_EVENT"},
    {MessageType::PING, "PING"},
    {MessageType::PONG, "PONG"},
};

json parseJson(const std::string& text, const char* what)
{
    try {
        return json::parse(text);
    }
    catch (const json::exception& e) {
        throw ProtocolError(std::string("bad JSON in ") + what + ": " + e.what());
    }
}

void expectFrames(const Frames& frames, size_t count, const char* what)
{
    if (frames.size() != count)
        throw ProtocolError(std::string(what) + " needs " + std::to_string(count)
                            + " frames, got " + std::to_string(frames.size()));
}

json spotsJson(const std::vector<AuctionSpot>& spots)
{
    json list = json::array();
    for (const auto& s : spots)
        list.push_back(json{{"spot", s.spotIndex}, {"creatives", s.creatives}});
    return list;
}

std::vector<AuctionSpot> spotsFrom(const json& j)
{
    std::vector<AuctionSpot> spots;
    try {
        for (const auto& entry : j)
            spots.push_back({entry.at("spot").get<int>(),
                             entry.at("creatives").get<std::vector<int>>()});
    }
    catch (const json::exception& e) {
        throw ProtocolError(std::string("bad AUCTION spots: ") + e.what());
    }
    return spots;
}

// Runs fn and turns the exceptions of the JSON readers into ProtocolError.
template<typename Fn>
auto wrapped(Fn&& fn)
{
    try {
        return fn();
    }
    catch (const ProtocolError&) {
        throw;
    }
    catch (const std::exception& e) {
        throw ProtocolError(e.what());
    }
}

struct Encoder {
    Frames operator()(const ConfigMsg& m) const
    {
        return {"CONFIG", m.agent, m.config.toJson().dump()};
    }
    Frames operator()(const GotConfigMsg& m) const { return {"GOTCONFIG", m.agent}; }
    Frames operator()(const NeedConfigMsg&) const { return {"NEEDCONFIG"}; }
    Frames operator()(const AuctionMsg& m) const
    {
        if (!m.request) throw ProtocolError("AUCTION without a bid request");
        return {"AUCTION", m.auctionId.toString(), std::format("{}", m.timeLeftMs),
                toJson(*m.request).dump(), spotsJson(m.spots).dump()};
    }
    Frames operator()(const BidMsg& m) const
    {
        return {"BID", m.auctionId.toString(), m.bids.toJson().dump()};
    }
    Frames operator()(const NoBudgetMsg& m) const
    {
        return {"NOBUDGET", m.auctionId.toString()};
    }
    Frames operator()(const TooLateMsg& m) const
    {
        return {"TOOLATE", m.auctionId.toString()};
    }
    Frames operator()(const InvalidMsg& m) const
    {
        return {"INVALID", m.auctionId.toString(), m.reason};
    }
    Frames operator()(const ErrorMsg& m) const
    {
        return {"ERROR", m.reason, m.auctionId.toString()};
    }
    Frames operator()(const WinMsg& m) const { return {"WIN", m.event.toJson().dump()}; }
    Frames operator()(const LossMsg& m) const { return {"LOSS", m.event.toJson().dump()}; }
    Frames operator()(const CampaignEventMsg& m) const
    {
        return {"CAMPAIGN_EVENT", m.event.toJson().dump()};
    }
    Frames operator()(const PingMsg& m) const { return {"PING", m.token}; }
    Frames operator()(const PongMsg& m) const { return {"PONG", m.token}; }
};

} // namespace

std::string toString(MessageType type)
{
    for (const auto& entry : TYPE_NAMES)
        if (entry.type == type) return entry.name;
    return "UNKNOWN";
}

MessageType parseMessageType(const std::string& name)
{
    for (const auto& entry : TYPE_NAMES)
        if (name == entry.name) return entry.type;
    throw ProtocolError("unknown message type " + name);
}

MessageType typeOf(const Message& message)
{
    static_assert(std::variant_size_v<Message> == 14, "keep types[] in sync");
    static constexpr MessageType types[] = {
        MessageType::CONFIG, MessageType::GOTCONFIG, MessageType::NEEDCONFIG,
        MessageType::AUCTION, MessageType::BID,
        MessageType::NOBUDGET, MessageType::TOOLATE, MessageType::INVALID,
        MessageType::ERROR,
        MessageType::WIN, MessageType::LOSS, MessageType::CAMPAIGN_EVENT,
        MessageType::PING, MessageType::PONG};
    return types[message.index()];   // same order as the variant
}

Frames encode(const Message& message)
{
    return std::visit(Encoder{}, message);
}

Message decode(const Frames& frames)
{
    if (frames.empty()) throw ProtocolError("empty message");

    switch (parseMessageType(frames[0])) {
    case MessageType::CONFIG:
        expectFrames(frames, 3, "CONFIG");
        return wrapped([&]() -> Message {
            return ConfigMsg{frames[1],
                             AgentConfig::fromJson(parseJson(frames[2], "CONFIG"))};
        });
    case MessageType::GOTCONFIG:
        expectFrames(frames, 2, "GOTCONFIG");
        return GotConfigMsg{frames[1]};
    case MessageType::NEEDCONFIG:
        expectFrames(frames, 1, "NEEDCONFIG");
        return NeedConfigMsg{};
    case MessageType::AUCTION:
        expectFrames(frames, 5, "AUCTION");
        return wrapped([&]() -> Message {
            AuctionMsg m;
            m.auctionId = Id(frames[1]);
            try {
                m.timeLeftMs = std::stod(frames[2]);
            }
            catch (const std::exception&) {
                throw ProtocolError("bad AUCTION time left " + frames[2]);
            }
            m.request = std::make_shared<BidRequest>(
                    bidRequestFromJson(parseJson(frames[3], "AUCTION request")));
            m.spots = spotsFrom(parseJson(frames[4], "AUCTION spots"));
            return m;
        });
    case MessageType::BID:
        expectFrames(frames, 3, "BID");
        return wrapped([&]() -> Message {
            return BidMsg{Id(frames[1]), Bids::fromJson(parseJson(frames[2], "BID"))};
        });
    case MessageType::NOBUDGET:
        expectFrames(frames, 2, "NOBUDGET");
        return NoBudgetMsg{Id(frames[1])};
    case MessageType::TOOLATE:
        expectFrames(frames, 2, "TOOLATE");
        return TooLateMsg{Id(frames[1])};
    case MessageType::INVALID:
        expectFrames(frames, 3, "INVALID");
        return InvalidMsg{Id(frames[1]), frames[2]};
    case MessageType::ERROR:
        expectFrames(frames, 3, "ERROR");
        return ErrorMsg{frames[1], Id(frames[2])};
    case MessageType::WIN:
        expectFrames(frames, 2, "WIN");
        return wrapped([&]() -> Message {
            return WinMsg{MatchedWinLoss::fromJson(parseJson(frames[1], "WIN"))};
        });
    case MessageType::LOSS:
        expectFrames(frames, 2, "LOSS");
        return wrapped([&]() -> Message {
            return LossMsg{MatchedWinLoss::fromJson(parseJson(frames[1], "LOSS"))};
        });
    case MessageType::CAMPAIGN_EVENT:
        expectFrames(frames, 2, "CAMPAIGN_EVENT");
        return wrapped([&]() -> Message {
            return CampaignEventMsg{
                MatchedCampaignEvent::fromJson(parseJson(frames[1], "CAMPAIGN_EVENT"))};
        });
    case MessageType::PING:
        expectFrames(frames, 2, "PING");
        return PingMsg{frames[1]};
    case MessageType::PONG:
        expectFrames(frames, 2, "PONG");
        return PongMsg{frames[1]};
    }
    throw ProtocolError("unreachable");
}

} // namespace RTBKIT::protocol

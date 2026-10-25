// Wire protocol between the router and bidding agents, like the original
// router <-> agent ZeroMQ messages but with JSON payloads.
//
// Transport (done by the router / agent client, not here):
//   router: ZMQ_ROUTER socket, bind   tcp://127.0.0.1:<agentPort>
//   agent:  ZMQ_DEALER socket, connect to it, ZMQ_IDENTITY = agent name
// A message is a list of frames (strings). The router sees an extra first
// frame, the agent identity, that is NOT part of the message encoded here.
// Frame 0 is always the message type name.
//
//   agent  -> router   CONFIG   agent, AgentConfig JSON
//   router -> agent    GOTCONFIG agent                     (config accepted)
//                      NEEDCONFIG                          (router has no config for you: send CONFIG)
//                      ERROR    reason, auctionId?         (e.g. config rejected)
//   router -> agent    AUCTION  auctionId, timeLeftMs, BidRequest JSON, spots JSON
//   agent  -> router   BID      auctionId, Bids JSON       (empty "bids" = pass)
//   router -> agent    NOBUDGET auctionId                  (bid refused by the banker)
//                      TOOLATE  auctionId                  (bid arrived after the deadline)
//                      INVALID  auctionId, reason          (bid does not fit the auction)
//   router -> agent    WIN | LOSS   MatchedWinLoss JSON
//                      CAMPAIGN_EVENT MatchedCampaignEvent JSON
//   either way         PING token / PONG token
//
// timeLeftMs is relative so that no clock has to be shared. Spots JSON is
// [{"spot": 0, "creatives": [0, 2]}, ...]: the agent may only bid on these
// spot / creative index pairs (the result of filtering).
#pragma once

#include "agent/agent_config.h"
#include "common/auction.h"
#include "common/bid_request.h"
#include "common/events.h"
#include "common/id.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace RTBKIT::protocol {

/** Thrown by decode for malformed messages. */
struct ProtocolError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class MessageType {
    CONFIG, GOTCONFIG, NEEDCONFIG,
    AUCTION, BID,
    NOBUDGET, TOOLATE, INVALID, ERROR,
    WIN, LOSS, CAMPAIGN_EVENT,
    PING, PONG
};

/** "CONFIG", "AUCTION", ... */
std::string toString(MessageType type);

/** Throws ProtocolError for an unknown name. */
MessageType parseMessageType(const std::string& name);


struct ConfigMsg {
    std::string agent;
    AgentConfig config;
};

struct GotConfigMsg {
    std::string agent;
};

struct NeedConfigMsg {};

/** One biddable spot and the creatives of the agent that fit it. */
struct AuctionSpot {
    int spotIndex = 0;
    std::vector<int> creatives;

    bool operator==(const AuctionSpot&) const = default;
};

struct AuctionMsg {
    Id auctionId;
    double timeLeftMs = 0;
    std::shared_ptr<const BidRequest> request;
    std::vector<AuctionSpot> spots;
};

struct BidMsg {
    Id auctionId;
    Bids bids;
};

struct NoBudgetMsg { Id auctionId; };
struct TooLateMsg { Id auctionId; };

struct InvalidMsg {
    Id auctionId;
    std::string reason;
};

struct ErrorMsg {
    std::string reason;
    Id auctionId;   ///< empty if the error is not about an auction
};

struct WinMsg { MatchedWinLoss event; };
struct LossMsg { MatchedWinLoss event; };
struct CampaignEventMsg { MatchedCampaignEvent event; };

struct PingMsg { std::string token; };
struct PongMsg { std::string token; };

using Message = std::variant<
    ConfigMsg, GotConfigMsg, NeedConfigMsg,
    AuctionMsg, BidMsg,
    NoBudgetMsg, TooLateMsg, InvalidMsg, ErrorMsg,
    WinMsg, LossMsg, CampaignEventMsg,
    PingMsg, PongMsg>;

using Frames = std::vector<std::string>;

MessageType typeOf(const Message& message);

/** Pure: no sockets involved. */
Frames encode(const Message& message);

/** Throws ProtocolError for unknown types, wrong frame counts and bad JSON. */
Message decode(const Frames& frames);

} // namespace RTBKIT::protocol

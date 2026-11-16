// Creates a banker from JSON config:
//   {"type": "memory" | "null" | "redis",
//    "accounts": {"campaign:strategy": {"budget": "100USD"}, ...},
//    // redis only:
//    "host": "127.0.0.1", "port": 6379, "prefix": "rtb",
//    "syncIntervalMs": 1000,   // background round: report spend, top up budget
//    "spendRate": "100000USD/1M",  // allocation taken per second and account
//    "ioTimeoutMs": 500, "maxSettledItems": 1000000}
// The redis banker keeps a local budget allocation, so bidding never waits on
// Redis, and does not throw if Redis is down (see redis_banker.h); setting a
// budget from "accounts" needs Redis and throws if it can not be reached.
// "type" defaults to "memory"; null or {} gives an empty memory banker.
// A budget is anything Amount's JSON accepts ("100USD", [100, "USD/1M"]) or a
// plain number of USD micro-units. An account without "budget" is only
// created. "null" ignores the accounts.
// Throws std::runtime_error for a bad config, or for "redis" when the build
// has no RTB_WITH_REDIS.
#pragma once

#include "banker/banker.h"

namespace RTBKIT {

std::unique_ptr<Banker> makeBanker(const nlohmann::json& config);

} // namespace RTBKIT

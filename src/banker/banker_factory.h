// Creates a banker from JSON config:
//   {"type": "memory" | "null" | "redis",
//    "accounts": {"campaign:strategy": {"budget": "100USD"}, ...},
//    // redis only:
//    "host": "127.0.0.1", "port": 6379, "prefix": "rtb"}
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

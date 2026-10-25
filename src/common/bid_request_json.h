// Native JSON form of a BidRequest. This is what travels between the router
// and the bidding agents (it is NOT OpenRTB; openrtb_parser.h reads OpenRTB).
// Round trip: bidRequestFromJson(toJson(r)) is equal to r field by field.
#pragma once

#include "common/bid_request.h"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace RTBKIT {

/** Dates travel as integer milliseconds since the Unix epoch. */
int64_t dateToMs(Date date);
Date dateFromMs(int64_t ms);

nlohmann::json toJson(const BidRequest& request);

/** Throws std::runtime_error on malformed input. */
BidRequest bidRequestFromJson(const nlohmann::json& j);

// ADL hooks so that `json j = request;` and `j.get<BidRequest>()` work.
void to_json(nlohmann::json& j, const BidRequest& request);
void from_json(const nlohmann::json& j, BidRequest& request);

} // namespace RTBKIT

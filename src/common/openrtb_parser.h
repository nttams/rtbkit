// OpenRTB 2.x JSON -> BidRequest, following
// rtbkit/plugins/bid_request/openrtb_bid_request.cc.
#pragma once

#include "common/bid_request.h"

#include <chrono>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace RTBKIT {

/** Thrown for malformed JSON or an invalid OpenRTB request. */
struct OpenRtbError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct OpenRtbOptions {
    /** Exchange name stored in BidRequest::exchange. Falls back to the
        provider name when empty. */
    std::string exchange;
    std::string provider;

    /** Used as BidRequest::timestamp; defaults to the current time. */
    std::optional<Date> now;
};

/** Converts an OpenRTB 2.x bid request.

    Covers: id, at, tmax, cur, bcat, badv, wseat, imp (banner w/h/format/pos/
    api, video, bidfloor/bidfloorcur, tagid, instl, pmp), site/app (page,
    bundle, domain, cat, publisher), device (ip, ipv6, ua, language, geo),
    user (id, buyeruid, geo, data segments). Fields we do not use are
    ignored. Throws OpenRtbError. */
BidRequest parseOpenRtbBidRequest(const nlohmann::json& json,
                                  const OpenRtbOptions& options = {});

BidRequest parseOpenRtbBidRequestText(std::string_view body,
                                      const OpenRtbOptions& options = {});

} // namespace RTBKIT

// Pieces of the mock exchange (apps/mock_exchange) that are worth testing:
// the random OpenRTB request generator and small URL / statistics helpers.
#pragma once

#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace RTBKIT {

struct MockRequestOptions {
    int tmaxMs = 100;
    double maxFloorCpm = 1.0;        ///< floors are 0 or in (0, maxFloorCpm]
    double noFloorProbability = 0.3;
};

/** A realistic random OpenRTB 2.x bid request: unique id, 1 or 2 banner
    imps (300x250 or 728x90, sometimes both formats), site url / domain,
    device ip / language / geo country, user id and buyeruid, bidfloor, tmax.
    The same seed gives the same sequence (the ids carry a counter). */
nlohmann::json generateOpenRtbRequest(std::mt19937_64& rng,
                                      const MockRequestOptions& options = {});

/** "http://host:port/path?query" -> {"http://host:port", "/path?query"}.
    The path is "/" when the url has none. Throws std::invalid_argument when
    the url has no scheme and host. */
std::pair<std::string, std::string> splitUrl(const std::string& url);

/** Replaces every ${AUCTION_PRICE} in url by the CPM price. */
std::string replaceAuctionPrice(const std::string& url, double priceCpm);

/** Percent encodes everything but letters, digits and -_.~ */
std::string urlEncode(const std::string& s);

/** Value of a query string parameter of a url (percent decoded), empty if
    absent. */
std::string queryParam(const std::string& url, const std::string& name);

/** p in [0, 1]. Sorts the vector. Returns 0 for an empty one. */
double percentile(std::vector<double>& values, double p);

} // namespace RTBKIT

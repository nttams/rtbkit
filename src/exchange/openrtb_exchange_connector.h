// OpenRTB 2.x exchange connector over HTTP (asio + llhttp), following
// rtbkit/plugins/exchange/openrtb_exchange_connector.{h,cc}.
//
//   POST /auctions   body: OpenRTB 2.x bid request (JSON)
//     200 + OpenRTB bid response   at least one spot got a bid
//     204                          no bid (no response, or router too late)
//     400 {"error": ...}           malformed request
//     500 {"error": ...}           internal error (e.g. no auction handler)
//   GET /ping        -> 200 "pong"
//
// Asynchronous, like the original rtbkit HttpExchangeConnector: a few event
// loop threads serve every connection. A thread parses the request, creates
// the Auction (expiry = time of the request's first byte + tmax), calls
// submit() and goes back to other connections at once; no thread waits for
// the auction. When the router finishes it, the completion callback posts
// the reply onto the connection; a timer answers 204 at expiry + graceMs if
// the router never does. Keep-alive and pipelined requests are served in
// order, one auction at a time per connection.
//
// configure() keys (all optional):
//   "name"          exchange name, default "openrtb" (providerConfig.<name> of
//                   a creative is read by getCreativeProviderData)
//   "host"          listen address, default "127.0.0.1"
//   "port"          default 12350, 0 = pick a free port (see port())
//   "threads"       event loop threads, default 2 (numThreads in the original).
//                   Not a limit on open auctions or connections.
//   "idleTimeoutMs" close a connection idle (or sending a request) this long,
//                   default 5000
//   "maxBodyBytes"  larger requests are answered 413, default 1048576
//   "winNoticeUrl"  default "http://127.0.0.1:12360/win"
//   "maxTmaxMs"     upper bound for the request's tmax, default 1000
//   "graceMs"       extra wait after the expiry, default 50
//
// A creative's providerConfig.<name> may contain "adm" (ad markup), "crid"
// (creative id on the wire, default the creative id) and "adomain" (array of
// strings). Any other type for these keys makes the creative incompatible.
#pragma once

#include "exchange/exchange_connector.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace RTBKIT {

class OpenRtbExchangeConnector : public ExchangeConnector {
public:
    explicit OpenRtbExchangeConnector(std::string name = "openrtb");
    ~OpenRtbExchangeConnector() override;

    void configure(const nlohmann::json& config) override;
    void start() override;
    void stop() override;
    int port() const override { return port_; }

    std::shared_ptr<void> getCreativeProviderData(
            const AgentConfig& config, const Creative& creative) const override;

    struct Stats {
        uint64_t requests = 0;   ///< POST /auctions received
        uint64_t bids = 0;       ///< answered 200
        uint64_t noBids = 0;     ///< answered 204 (includes timeouts and expiredInQueue)
        uint64_t timeouts = 0;   ///< 204 because the auction did not finish in time
        uint64_t expiredInQueue = 0; ///< 204 at once: tmax already over when the request was read
        uint64_t badRequests = 0;///< answered 400
        uint64_t errors = 0;     ///< answered 500
    };
    Stats stats() const;

    /** The OpenRTB bid response for a finished auction, or an empty string
        when no spot has a bid. Public so that it can be tested alone. */
    std::string buildBidResponse(const Auction& auction) const;

    const std::string& winNoticeUrl() const { return winNoticeUrl_; }

private:
    struct Server;
    class Session;

    /** Parses the body and submits the auction. Returns it, or null with
        status / responseBody set when the request is answered at once. */
    std::shared_ptr<Auction> beginAuction(const std::string& body,
                                          Auction::TimePoint firstData,
                                          Auction::Completion onFinished,
                                          int& status, std::string& responseBody);

    /** The answer for an auction that finished (or was cut at the deadline). */
    void endAuction(const Auction& auction, bool timedOut,
                    int& status, std::string& responseBody);

    std::string host_ = "127.0.0.1";
    int configuredPort_ = 12350;
    int port_ = 0;
    int threads_ = 2;
    double idleTimeoutMs_ = 5000;
    size_t maxBodyBytes_ = 1 << 20;
    std::string winNoticeUrl_ = "http://127.0.0.1:12360/win";
    double maxTmaxMs_ = 1000;
    double graceMs_ = 50;

    std::shared_ptr<Server> server_;

    std::atomic<uint64_t> requests_{0}, bids_{0}, noBids_{0}, timeouts_{0},
                          expiredInQueue_{0}, badRequests_{0}, errors_{0};
};

} // namespace RTBKIT

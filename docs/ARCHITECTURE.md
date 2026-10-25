# Architecture and contracts (modern RTBKit)

Read this before changing anything outside your own folder. The headers listed
here are the contracts between components; change them only by agreement.
C++23, namespace `RTBKIT`, includes rooted at `src/` (`#include "common/auction.h"`).

## Components

| Component | Code | What it does |
|---|---|---|
| Exchange connector | `src/exchange` | HTTP server speaking OpenRTB. Parses a request into a `BidRequest`, makes an `Auction`, gives it to the router, writes the HTTP reply when the auction finishes. |
| Router | `src/router` | Filters agent configs (`FilterPool`), sends the auction to matching agents, collects bids, asks the banker, picks the winner per spot, finishes the auction. |
| Bidding agent | `src/agent`, `apps/fixed_price_agent` | Separate process or thread. Sends its `AgentConfig`, answers `AUCTION` with `BID`. |
| Banker | `src/banker` | Budgets per account. Reserves money before a bid goes out, settles on win. |
| Post auction | `src/post_auction` | Matches win / loss / click events to submitted bids, settles with the banker, tells the router what to tell the agents. |
| Ad server | `src/adserver`, `apps/adserver` | HTTP endpoint for win notices, impressions, clicks. Feeds the post auction service. |
| Mock exchange | `apps/mock_exchange` | Load generator: sends OpenRTB requests, reads responses, calls the win notice URL. |

## Flow

```
mock_exchange          exchange connector            router              agent(s)         banker      post auction     adserver
     |  POST /openrtb       |                           |                    |                |              |              |
     |--------------------->| parse -> BidRequest       |                    |                |              |              |
     |                      | Auction::create(cb)       |                    |                |              |              |
     |                      |--- AuctionHandler ------->| FilterPool::filter |                |              |              |
     |                      |                           |--- AUCTION ------->|                |              |              |
     |                      |                           |<--- BID -----------|                |              |              |
     |                      |                           |-- authorizeBid ---------------------->|              |              |
     |                      |                           | auction->setResponse                  |              |              |
     |                      |                           | (all answered or expiry)              |              |              |
     |                      |                           |-- cancelBid (losers) ---------------->|              |              |
     |                      |                           |-- SubmittedAuctionEvent ------------------------->|              |
     |                      |<-- cb: auction finished --| auction->finish()  |                |              |              |
     |<-- 200 bid response -| (nurl = adserver /win)    |                    |                |              |              |
     |-- GET /win?price=... ------------------------------------------------------------------------------------------------->|
     |                      |                           |                    |                |              |<-PostAuctionEvent
     |                      |                           |                    |                |<- winBid ----|              |
     |                      |                           |<-- MatchedWinLoss -------------------------------|              |
     |                      |                           |--- WIN ----------->|                |              |              |
```

Exchange with no bids: the router still finishes the auction at its expiry and
the connector answers 204.

## Threads

- **Router: one event loop thread.** It alone touches the `FilterPool`
  (not thread safe), the agent table and the auctions in flight. Everything else
  hands work to it through a queue (`Router::injectAuction`,
  `onMatchedWinLoss`, ...) or the zmq socket (the socket is only used by the
  loop thread; use a `zmq::socket_t` per thread or an inproc pipe).
  An exception in the loop (a banker that throws, a bad connector config) is
  caught, logged and counted in the router `errors` stat; it never ends the
  process. A banker failure while accepting a bid means `NOBUDGET`; at the end
  of an auction every banker call is guarded, so `auction->finish()` always runs.
- **Exchange connector: own HTTP threads** (httplib). Each request thread calls
  `submit(auction)` then blocks on the result. The completion callback runs on
  the router thread: it must only fulfil a promise / condition variable.
- **Ad server: own HTTP threads.** Calls `PostAuctionService::onEvent`.
- **Post auction:** methods are thread safe; `tick(now)` is called by a timer
  thread (or the router loop).
- **Banker:** thread safe (called from router and post auction threads).
- **Auction:** thread safe (mutex inside).
- Agents are separate processes (or threads in the demo) that talk to the router
  only over ZeroMQ.

## Units

All prices are `Amount` per impression in micro-units of a currency:
`USD_CPM(1.5) == 1500 micro USD`. Bid prices, floors (`AdSpot::reservePrice`),
banker budgets and win prices use the same unit. OpenRTB prices are CPM in
currency units: convert with `Amount::fromCpm(code, cpm)` / `amount.toCpm()`.
Only USD works for now (mixing currencies throws). The router answers a bid in
another currency with `INVALID`.

## Contracts (headers)

| Header | Contract |
|---|---|
| `common/auction.h` | `Auction`, `Bid`, `Bids`, `Response`, `WinCostModel` |
| `common/bid_request_json.h` | `toJson(BidRequest)`, `bidRequestFromJson`, `dateToMs/dateFromMs` (native JSON, not OpenRTB) |
| `common/events.h` | `SubmittedAuctionEvent`, `PostAuctionEvent`, `MatchedWinLoss`, `MatchedCampaignEvent`, `bidItemId` |
| `agent/agent_config.h` | `AgentConfig::fromJson` / `toJson` |
| `agent/protocol.h` | router <-> agent messages, `encode` / `decode` |
| `banker/banker.h` | `Banker`, `AccountSummary`, `NullBanker` |
| `exchange/exchange_connector.h` | `ExchangeConnector`, `AuctionHandler` |
| `router/router.h` | `Router`, `RouterConfig` |
| `post_auction/post_auction.h` | `PostAuctionService` |
| `adserver/adserver.h` | `AdServer` (HTTP contract in the header) |
| `server/core_server.h` | `CoreServer`: banker + router + connector + post auction + ad server + admin, wired |

`ExchangeFilterHooks` (`common/exchange_filter_hooks.h`) is the small part of a
connector the filters see; `ExchangeConnector` derives from it.

### Auction life

1. Connector: `Auction::create(request, onFinished)`, then `submit(auction)`.
   Expiry = start + `request->timeAvailableMs`.
2. Router: per agent bid, `auction->setResponse(Response)` (priority first, then
   price; first wins a tie). The router sets `Response::config` so the connector
   finds the creative (`config->creatives[creativeIndex]`).
3. Router: `auction->finish()` once, by expiry at the latest. The completion
   callback then runs on the router thread.
4. Connector: for each spot, `auction->bestResponse(spot)`; none = no bid for the
   spot. Price on the wire: `WinCostModel` is for settlement; the bid response
   carries the response price (the max bid). Second price is applied at win.

### Banker rules

Item id is `bidItemId(auctionId, adSpotId, agent)` = `<len>:<auction>|<len>:<spot>|<agent>` (length prefixed, so ids holding `:` or `|` cannot collide; the post auction table key is the same without the agent), so two
agents with the same account never share a reservation. The post auction service
builds the same id from `Response::agent` of the submitted bid. Router: `authorizeBid(account,
item, bid price)` when a BID arrives (refused: send `NOBUDGET`, drop the bid).
After the auction: `cancelBid` for every response that did not win. The winner
stays reserved until the post auction service settles it (`winBid` with the win
price, `cancelBid` on loss or timeout, `forceWinBid` for a late win).
`Response::account` is the account to charge (the bid's own account, else the
config's).

### Agent protocol (ZeroMQ)

Router binds a `ROUTER` socket (default `tcp://127.0.0.1:12340`, from
`RouterConfig::agentEndpoint`); each agent connects a `DEALER` with
`ZMQ_IDENTITY = agent name`. The router sees `[identity, frame...]`; the agent
sees `[frame...]`. Frame 0 is the message type. `agent/protocol.h` encodes and
decodes frames (pure functions), sockets are the owner's job.

| Message | Direction | Frames after the type |
|---|---|---|
| `CONFIG` | agent -> router | agent name, AgentConfig JSON |
| `GOTCONFIG` | router -> agent | agent name |
| `NEEDCONFIG` | router -> agent | (none) router does not know you: send CONFIG |
| `AUCTION` | router -> agent | auction id, time left ms (relative), BidRequest JSON, spots JSON `[{"spot":0,"creatives":[0,1]}]` |
| `BID` | agent -> router | auction id, Bids JSON `{"bids":[{"spot","creative","price","priority","account"?,"bidData"?,"meta"?}]}` (empty list = pass) |
| `NOBUDGET` | router -> agent | auction id |
| `TOOLATE` | router -> agent | auction id |
| `INVALID` | router -> agent | auction id, reason |
| `ERROR` | router -> agent | reason, auction id (may be empty) |
| `WIN` / `LOSS` | router -> agent | MatchedWinLoss JSON |
| `CAMPAIGN_EVENT` | router -> agent | MatchedCampaignEvent JSON |
| `PING` / `PONG` | both | token |

The router replies `GOTCONFIG` to a good CONFIG and `ERROR` to a rejected one.
Before it filters a config, the router calls `ExchangeConnector::prepareConfig`
for each registered connector (filling `providerData`); without it the exchange
filters reject the config.

### Ports (defaults, all on 127.0.0.1, all configurable)

| Port | Who |
|---|---|
| 12340 | router agent endpoint (zmq ROUTER) |
| 12350 | exchange connector (OpenRTB HTTP `POST /auctions`) |
| 12360 | ad server (`/win`, `/impression`, `/click`, `/stats`) |
| 12370 | admin HTTP of `rtbkit_router` (`GET /stats`: router, connector, post auction, banker JSON) |

### Security

There is no authentication. The zmq agent identity is taken at face value (anyone
who can reach the router port can register as, and receive WIN/LOSS for, any agent
name), and the ad server `/win`, `/impression`, `/click` endpoints and the admin
HTTP are open. The post auction service limits the damage of a forged `/win`
(price is capped at the bid price, tables are size capped) but cannot stop it.
Trusted network only; the defaults bind 127.0.0.1.

### Win notice URL

The connector puts `nurl = <winNoticeUrl>?auctionId=<id>&adSpotId=<imp id>&price=${AUCTION_PRICE}`
in the OpenRTB bid. `winNoticeUrl` is a connector setting (default
`http://127.0.0.1:12360/win`). The exchange (mock) replaces `${AUCTION_PRICE}` with
the clearing price in CPM.

Price units end to end: the agent bids an `Amount` per impression (micro USD);
the connector writes `price = amount.toCpm()` in the OpenRTB bid; the exchange
calls the nurl with its clearing price in CPM; `/win` turns it back with
`Amount::fromCpm`; the post auction service charges the banker that amount.
**The exchange's clearing price wins**: the router does not apply `WinCostModel`
to the settlement, a second price auction simply clears below the bid and the
exchange says so in `${AUCTION_PRICE}`. (The router reserves the full bid price,
the difference is released at the win.) The price charged is capped at the bid
price: `min(win price, bid price)`; a notice above it is counted as `clamped` in
the post auction stats.

## Running it

- `build/bin/rtbkit_demo --requests 2000 --qps 500` runs everything in one
  process (`CoreServer` + two in-process `BiddingAgent`s + a load generator) and
  prints a summary; `--serve` keeps the services up.
- `scripts/run_local.sh` runs `rtbkit_router` (config/rtbkit.json), two
  `fixed_price_agent` processes (config/agents/*.json) and `mock_exchange` as
  separate processes, prints the stats and stops them.
- `tests/integration/end_to_end_test.cc` (`build/src/server/rtb_integration_tests`)
  runs the full stack in-process on random ports.

## Build layout

Each area has its own `src/<area>/CMakeLists.txt` (libraries, plus the area test
executable through `rtb_add_area_tests`). Tests live in `tests/<area>/*_test.cc`
and are globbed: a new test file needs no CMake change. Apps live in
`apps/<name>/` with their own `CMakeLists.txt` (use `rtb_add_app`); `apps/` globs
its sub folders. Shared fixtures: `tests/fixtures.h` (`sampleBidRequest`,
`sampleAgentConfig`, `sampleResponse`, `readTestFile`).

Libraries: `rtb_common`, `rtb_agent_config`, `rtb_agent_protocol`, `rtb_filters`,
`rtb_banker`, `rtb_exchange`, `rtb_router`, `rtb_post_auction`, `rtb_adserver`,
`rtb_server` (`src/server`, the wired core server). Executables go to `build/bin/`.
Test executables: `build/tests/rtb_tests` (shared, P1 + contracts) and
`build/src/<area>/rtb_<area>_tests`.

## Ownership (parallel work)

Edit only your own folders. Shared files (`CMakeLists.txt`, `cmake/`,
`src/CMakeLists.txt`, `src/common`, `src/filters`, `tests/CMakeLists.txt`,
`tests/fixtures.h`, `docs/`) are read-only for the parallel agents; ask the
integration owner if a contract must change.

| Owner | Folders |
|---|---|
| banker | `src/banker`, `tests/banker` |
| router + agents | `src/router`, `src/agent` (bidding agent client lib; `protocol.*` and `agent_config.*` stay as they are), `apps/fixed_price_agent`, `tests/router`, `tests/agent` |
| exchange | `src/exchange`, `apps/mock_exchange`, `tests/exchange` |
| post auction | `src/post_auction`, `src/adserver`, `apps/adserver`, `tests/post_auction`, `tests/adserver` |
| integration | `src/server`, `apps/rtbkit_demo`, `apps/rtbkit_router`, `scripts/`, `config/`, `tests/integration` |

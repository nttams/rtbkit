# RTBkit (modern)

A C++23 rewrite of [RTBkit](https://github.com/rtbkit/rtbkit), a real-time bidder for display ads.
It builds and runs on macOS (Apple clang, arm64). The old jml / soa / boost / zookeeper code base is gone.

## What it does

```
mock_exchange --OpenRTB--> exchange connector --> router --zmq--> bidding agents
                                                    |  \
                                                 banker  post auction <-- adserver (/win /impression /click)
```

- **Exchange connector** (`src/exchange`): OpenRTB 2.x over HTTP (`POST /auctions`, port 12350).
- **Router** (`src/router`): filters campaigns (static + creative filters), sends auctions to agents, collects bids, picks winners.
- **Bidding agents** (`src/agent`, `apps/fixed_price_agent`): talk to the router over ZeroMQ (port 12340).
- **Banker** (`src/banker`): budgets per account. In-memory by default, Redis optional.
- **Post auction** (`src/post_auction`) + **ad server** (`src/adserver`, port 12360): match win / loss / click events to bids and charge the banker.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full flow, threads, message formats and units.

## Build

```
brew install cmake ninja zeromq cppzmq nlohmann-json cpp-httplib catch2
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Redis banker (optional): `brew install hiredis redis`, then add `-DRTB_WITH_REDIS=ON`.
Its tests read `RTB_REDIS_HOST` / `RTB_REDIS_PORT` and skip when no server is reachable.

## Run

All in one process (services, two agents and load):

```
./build/bin/rtbkit_demo --requests 2000 --qps 500    # prints router/banker/agent stats
./build/bin/rtbkit_demo --serve                      # keep running until Ctrl-C
```

As separate processes (router, 2 agents, mock exchange):

```
scripts/run_local.sh [count] [qps]
```

Or by hand:

```
./build/bin/rtbkit_router --config config/rtbkit.json
./build/bin/fixed_price_agent -c config/agents/agent_a.json -r tcp://127.0.0.1:12340 -p 2.0
./build/bin/mock_exchange --url http://127.0.0.1:12350/auctions --qps 500 --count 2000 \
    --win-rate 0.3 --adserver http://127.0.0.1:12360
curl http://127.0.0.1:12370/stats
```

## Layout

| Path | What |
|---|---|
| `src/common` | ids, money (`Amount`), bid request, OpenRTB parser, auction, events |
| `src/filters` | static + creative filters, `FilterPool` |
| `src/agent` | agent config, router <-> agent protocol, `BiddingAgent` client |
| `src/router`, `src/banker`, `src/exchange`, `src/post_auction`, `src/adserver` | services |
| `src/server` | `CoreServer`: wires all services in one process |
| `apps/` | `rtbkit_demo`, `rtbkit_router`, `fixed_price_agent`, `mock_exchange`, `rtb_adserver` |
| `config/` | sample server and agent configs |
| `tests/` | Catch2 unit and end-to-end tests |

## Not ported (yet)

Other exchange connectors (only OpenRTB), augmentors, JS/Python bidders, the monitor and
analytics services, data logger, currencies other than USD.

Security: there is no authentication anywhere. The zmq agent identity (any process
that can reach the router port can connect as any agent name), the `/win`,
`/impression` and `/click` ad server endpoints and the admin HTTP are all
unauthenticated. Run it on a trusted network only (the defaults bind 127.0.0.1).

## License

Apache 2.0, see [LICENSE](LICENSE).

# static-filter-core

The static filter part of RTBKit, cut out into a small standalone C++23 project.
It has no jml, soa, boost or zmq. It builds with Apple clang and a plain Makefile.

## Flow

```
BidRequest ──> FilterPool.filter() ──> Result
                  │                      ├─ matched campaigns
                  │                      └─ rejected campaign -> filter name
                  ├─ synced campaigns (AgentConfig list, addConfig/removeConfig)
                  └─ static filters, run in priority order
```

1. Campaigns (`AgentConfig`) are added to a `FilterPool`.
2. A `BidRequest` comes in.
3. Each static filter narrows a `ConfigSet` (a bitset of campaigns still alive).
4. The result lists the campaigns that passed, plus the first filter that dropped each other campaign.

Static filters ported: Segments, UserPartition, HourOfWeek, Url, Host, Language,
Location, ExchangePre, ExchangePost, ExchangeName, FoldPosition, RequiredIds, LatLongDev.

## Build and run

```
make              # build/libstaticfilter.a
make test         # 8 tests ported from static_filters_test.cc
make demo
./build/demo examples/campaigns.json examples/bid_requests.json
```

The sample JSON is set up so that each static filter rejects at least one campaign.

## Mapping to original RTBKit files

| Here | Original |
|---|---|
| `src/filter.h` | `rtbkit/common/filter.h`, `filter.cc` |
| `src/static_filters.h/.cc` | `rtbkit/core/router/filters/static_filters.h/.cc` |
| `src/generic_filters.h` | `rtbkit/core/router/filters/generic_filters.h` (parts the static filters need) |
| `src/priority.h` | `rtbkit/core/router/filters/priority.h` |
| `src/filter_pool.h/.cc` | `rtbkit/core/router/filter_pool.h/.cc` (filter loop only, no events or threads) |
| `src/agent_config.h` | `rtbkit/core/agent_configuration/agent_config.h` (fields used by filters) |
| `src/bid_request.h` | `rtbkit/common/bid_request.h` (fields used by filters) |
| `src/segments.h` | `rtbkit/common/segments.h` |
| `src/include_exclude.h` | `jml/utils` / `soa` include-exclude helpers |
| `src/exchange_connector.h`, `src/exc_check.h` | Small stub of `rtbkit/common/exchange_connector.h` for ExchangePre/Post |
| `src/md5.h` | Replaces the crypto hash used by UserPartition |
| `tests/` | `rtbkit/core/router/filters/testing/static_filters_test.cc`, `utils.h` |

## Changes from the original

- `Date` → `std::chrono`; `Url`/`Utf8String` → `std::string` with a tiny URL helper;
  `boost::regex` → `std::regex`; `compact_vector` → `std::vector`.
- No router, exchange connectors, bidder, augmentors, banker or monitoring.
- No creative filters.

## Known behaviour differences

- `std::regex` (ECMAScript, byte-wise) replaces `boost::regex` / `boost::u32regex`.
  Perl-only syntax (e.g. lookbehind) is not supported, and non-ASCII text
  (mainly in the Location filter) is matched byte by byte.
- `Url::host()` is a small hand-written parser (lowercases, strips user info and port).
  Odd URLs may parse differently from `Datacratic::Url`.
- LatLongDev: a NaN lat/lon now counts as "no geo" (the original `== NaN` check never fired).
  The matched set is the same.
- `FilterPool` is single-threaded: no RCU/GC, no stats or event recording.
  Do not call `addConfig` and `filter` at the same time from different threads.
- Fixed an original bug in `FilterPool` where configs added before a filter were
  not all registered with that filter (`cfgId += next()` → `cfgId = next()`).
- UserPartition RANDOM uses a thread-local `std::mt19937_64` instead of `random()`.

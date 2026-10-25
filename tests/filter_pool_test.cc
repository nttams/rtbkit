// FilterPool: static filters + creative filters end to end.

#include "utils.h"
#include "filters/filter_pool.h"
#include "common/openrtb_parser.h"

#include <algorithm>
#include <chrono>

using namespace std;
using namespace RTBKIT;
using namespace RTBKIT::Test;

namespace {

// Exchange connector whose decisions come from the int stored in the
// provider data (0 means reject).
struct DemoConnector : public ExchangeFilterHooks
{
    std::string exchangeName() const override { return "demo"; }

    bool bidRequestPreFilter(const BidRequest&, const AgentConfig&,
                             const void* data) const override
    {
        return *static_cast<const int*>(data) != 0;
    }

    bool bidRequestCreativeFilter(const BidRequest&, const AgentConfig&,
                                  const void* data) const override
    {
        return *static_cast<const int*>(data) != 0;
    }
};

shared_ptr<void> providerData(int value)
{
    return make_shared<int>(value);
}

shared_ptr<AgentConfig> makeConfig(const string& name,
                                   const vector<Creative>& creatives)
{
    auto config = make_shared<AgentConfig>(name);
    config->account = AccountKey{"acme", name};
    config->creatives = creatives;
    config->providerData["demo"] = providerData(1);
    for (auto& creative : config->creatives)
        creative.providerData["demo"] = providerData(1);
    return config;
}

BidRequest makeRequest()
{
    BidRequest br;
    br.exchange = "demo";
    br.timestamp = chrono::system_clock::now();
    br.language = "en";
    br.url = Url("http://news.example.com/world");
    br.ipAddress = "203.0.113.9";
    br.userAgent = "test-agent";
    br.userIds.add(Id("user-1"), ID_EXCHANGE);
    br.userIds.add(Id("user-1"), ID_PROVIDER);
    addImp(br, AD_POS_ABOVE, { {300, 250}, {728, 90} });
    addImp(br, AD_POS_BELOW, { {160, 600} });
    return br;
}

vector<unsigned> creativesOf(const FilterPool::Match& m, unsigned spot)
{
    for (const auto& s : m.spots)
        if (s.spot == spot) return s.creatives;
    return {};
}

const FilterPool::Match* find(const FilterPool::Result& r, const string& name)
{
    for (const auto& m : r.matched)
        if (m.config == name) return &m;
    return nullptr;
}

string rejectedBy(const FilterPool::Result& r, const string& name)
{
    for (const auto& entry : r.rejected)
        if (entry.first == name) return entry.second;
    return "";
}

} // namespace

TEST_CASE("pool: default filters are sorted by priority")
{
    FilterPool pool;
    pool.initWithDefaultFilters();

    auto names = pool.getFilterNames();
    CHECK(names.size() == 20);
    CHECK(names.front() == "ExchangeName");
    CHECK(names.back() == "ExchangePost");

    auto pos = [&](const string& n) {
        return find(names.begin(), names.end(), n) - names.begin();
    };
    CHECK(pos("CreativeFormat") < pos("Segments"));
    CHECK(pos("Segments") < pos("CreativeSegments"));
    CHECK(pos("CreativeSegments") < pos("CreativeExchange"));
}

TEST_CASE("pool: unknown filter name and missing filters throw")
{
    FilterPool pool;
    CHECK_THROWS_AS(pool.addFilter("NoSuchFilter"), std::runtime_error);

    BidRequest br = makeRequest();
    CHECK_THROWS_AS(pool.filter(br, nullptr), std::runtime_error);
}

TEST_CASE("pool: matches report eligible creatives per spot")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;

    // c0: leaderboard, big box and a wildcard; c1: only a skyscraper.
    pool.addConfig("c0", makeConfig("c0", { Creative(728, 90, "lb", 0),
                                            Creative(300, 250, "bb", 1),
                                            Creative(0, 0, "any", 2) }));
    pool.addConfig("c1", makeConfig("c1", { Creative(160, 600, "sky", 0) }));

    BidRequest br = makeRequest();
    auto result = pool.filter(br, &conn);

    REQUIRE(result.matched.size() == 2);
    auto* c0 = find(result, "c0");
    auto* c1 = find(result, "c1");
    REQUIRE(c0);
    REQUIRE(c1);

    // Spot 0 accepts 300x250 and 728x90, spot 1 accepts 160x600.
    CHECK(creativesOf(*c0, 0) == vector<unsigned>{ 0, 1, 2 });
    CHECK(creativesOf(*c0, 1) == vector<unsigned>{ 2 });
    CHECK(creativesOf(*c1, 0).empty());
    CHECK(creativesOf(*c1, 1) == vector<unsigned>{ 0 });

    // c1 only has a spot 1 entry; spots without creatives are absent.
    CHECK(c1->spots.size() == 1);
    CHECK(result.rejected.empty());
}

TEST_CASE("pool: rejections name the filter that dropped the config")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;

    pool.addConfig("ok", makeConfig("ok", { Creative(300, 250, "bb", 0) }));

    auto wrongLang = makeConfig("lang", { Creative(300, 250, "bb", 0) });
    wrongLang->languageFilter.include.push_back(CachedRegex("^fr"));
    pool.addConfig("lang", wrongLang);

    auto wrongHost = makeConfig("host", { Creative(300, 250, "bb", 0) });
    wrongHost->hostFilter.exclude.push_back(DomainMatcher("example.com"));
    pool.addConfig("host", wrongHost);

    auto wrongExchange = makeConfig("xchg", { Creative(300, 250, "bb", 0) });
    wrongExchange->exchangeFilter.include.push_back("other");
    pool.addConfig("xchg", wrongExchange);

    pool.addConfig("size", makeConfig("size", { Creative(120, 60, "tiny", 0) }));

    auto crLang = makeConfig("crlang", { Creative(300, 250, "bb", 0) });
    crLang->creatives[0].languageFilter.include.push_back("fr");
    pool.addConfig("crlang", crLang);

    auto preFilter = makeConfig("pre", { Creative(300, 250, "bb", 0) });
    preFilter->providerData["demo"] = providerData(0);
    pool.addConfig("pre", preFilter);

    auto crExchange = makeConfig("crxchg", { Creative(300, 250, "bb", 0) });
    crExchange->creatives[0].providerData["demo"] = providerData(0);
    pool.addConfig("crxchg", crExchange);

    BidRequest br = makeRequest();
    auto result = pool.filter(br, &conn);

    CHECK(result.matchedNames() == vector<string>{ "ok" });
    CHECK(rejectedBy(result, "lang") == "Language");
    CHECK(rejectedBy(result, "host") == "Host");
    CHECK(rejectedBy(result, "xchg") == "ExchangeName");
    CHECK(rejectedBy(result, "size") == "CreativeFormat");
    CHECK(rejectedBy(result, "crlang") == "CreativeLanguage");
    CHECK(rejectedBy(result, "pre") == "ExchangePre");
    CHECK(rejectedBy(result, "crxchg") == "CreativeExchange");
    CHECK(result.rejected.size() == 7);
}

TEST_CASE("pool: a creative is dropped without dropping the config")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;

    auto config = makeConfig("c", { Creative(300, 250, "fr", 0),
                                    Creative(300, 250, "en", 1) });
    config->creatives[0].languageFilter.include.push_back("fr");
    config->creatives[1].languageFilter.exclude.push_back("fr");
    pool.addConfig("c", config);

    BidRequest br = makeRequest();
    auto result = pool.filter(br, &conn);

    REQUIRE(result.matched.size() == 1);
    CHECK(creativesOf(result.matched[0], 0) == vector<unsigned>{ 1 });
}

TEST_CASE("pool: deals and segments are applied per creative")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;

    auto config = makeConfig("c", { Creative(300, 250, "open", 0),
                                    Creative(300, 250, "deal", 1, "DEAL1"),
                                    Creative(300, 250, "sports", 2) });
    Creative::SegmentInfo seg;
    seg.include.add("sports");
    seg.excludeIfNotPresent = true;
    config->creatives[2].segments["interest"] = seg;
    pool.addConfig("c", config);

    BidRequest br = makeRequest();
    br.imp.resize(1);

    SECTION("open auction without segments")
    {
        auto result = pool.filter(br, &conn);
        REQUIRE(result.matched.size() == 1);
        CHECK(creativesOf(result.matched[0], 0) == vector<unsigned>{ 0 });
    }

    SECTION("private auction with the deal and the segment")
    {
        Pmp pmp;
        pmp.privateAuction = 1;
        pmp.deals.emplace_back();
        pmp.deals.back().id = Id("DEAL1");
        br.imp[0].pmp = pmp;
        br.segments.addStrings("interest", { "sports" });

        auto result = pool.filter(br, &conn);
        REQUIRE(result.matched.size() == 1);
        CHECK(creativesOf(result.matched[0], 0) == vector<unsigned>{ 1 });
    }

    SECTION("open auction with the segment")
    {
        br.segments.addStrings("interest", { "sports" });
        auto result = pool.filter(br, &conn);
        REQUIRE(result.matched.size() == 1);
        CHECK(creativesOf(result.matched[0], 0) == vector<unsigned>{ 0, 2 });
    }
}

TEST_CASE("pool: add, replace and remove configs")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;
    BidRequest br = makeRequest();

    unsigned a = pool.addConfig("a", makeConfig("a", { Creative(300, 250, "x", 0) }));
    unsigned b = pool.addConfig("b", makeConfig("b", { Creative(300, 250, "x", 0) }));
    CHECK(a != b);
    CHECK(pool.filter(br, &conn).matched.size() == 2);

    // Replacing keeps one entry for the name.
    pool.addConfig("a", makeConfig("a", { Creative(120, 60, "tiny", 0) }));
    auto result = pool.filter(br, &conn);
    CHECK(result.matchedNames() == vector<string>{ "b" });
    CHECK(rejectedBy(result, "a") == "CreativeFormat");

    pool.removeConfig("b");
    result = pool.filter(br, &conn);
    CHECK(result.matched.empty());
    CHECK(rejectedBy(result, "b").empty());

    // The freed slot is reused.
    unsigned c = pool.addConfig("c", makeConfig("c", { Creative(300, 250, "x", 0) }));
    CHECK(c == b);
    CHECK(pool.filter(br, &conn).matchedNames() == vector<string>{ "c" });

    pool.removeFilter("CreativeFormat");
    CHECK(pool.filter(br, &conn).matched.size() == 2);
}

TEST_CASE("pool: the mask limits the configs considered")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;
    BidRequest br = makeRequest();

    pool.addConfig("a", makeConfig("a", { Creative(300, 250, "x", 0) }));
    unsigned b = pool.addConfig("b", makeConfig("b", { Creative(300, 250, "x", 0) }));

    ConfigSet mask;
    mask.set(b);
    auto result = pool.filter(br, &conn, mask);
    CHECK(result.matchedNames() == vector<string>{ "b" });
    CHECK(result.rejected.empty());
}

TEST_CASE("pool: a request without exchange connector is rejected")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    pool.addConfig("a", makeConfig("a", { Creative(300, 250, "x", 0) }));

    BidRequest br = makeRequest();
    auto result = pool.filter(br, nullptr);
    CHECK(result.matched.empty());
    CHECK(rejectedBy(result, "a") == "ExchangePre");
}

TEST_CASE("pool: a request without impressions has nothing to bid on")
{
    FilterPool pool;
    pool.initWithDefaultFilters();
    DemoConnector conn;
    pool.addConfig("a", makeConfig("a", { Creative(300, 250, "x", 0) }));

    BidRequest br = makeRequest();
    br.imp.clear();
    auto result = pool.filter(br, &conn);
    CHECK(result.matched.empty());
    CHECK(!result.rejected.empty());
}

TEST_CASE("pool: parsed JSON config and OpenRTB request")
{
    auto config = make_shared<AgentConfig>(AgentConfig::parse(R"({
        "account": ["hello", "world"],
        "bidProbability": 0.5,
        "creatives": [
            { "id": 0, "format": "300x250", "name": "BigBox" },
            { "id": 1, "format": "728x90", "name": "LeaderBoard",
              "languageFilter": { "include": ["fr"] } },
            { "id": 2, "format": "160x600", "name": "Sky" }
        ],
        "locationFilter": { "include": ["^US:"] },
        "exchangeFilter": { "include": ["demo"] }
    })"));
    config->providerData["demo"] = providerData(1);
    for (auto& c : config->creatives) c.providerData["demo"] = providerData(1);

    FilterPool pool;
    pool.initWithDefaultFilters();
    pool.addConfig("bidder", config);
    DemoConnector conn;

    OpenRtbOptions opts;
    opts.exchange = "demo";
    BidRequest br = parseOpenRtbBidRequestText(R"({
        "id": "req-1",
        "imp": [ { "id": "1", "banner": { "w": 300, "h": 250, "pos": 1 } },
                 { "id": "2", "banner": { "w": 728, "h": 90 } } ],
        "site": { "page": "http://site.example.com/a" },
        "device": { "ip": "1.2.3.4", "ua": "agent", "language": "en",
                    "geo": { "country": "US", "region": "CA", "city": "SF",
                             "zip": "94105" } }
    })", opts);

    auto result = pool.filter(br, &conn);
    REQUIRE(result.matched.size() == 1);

    // 728x90 needs French; 160x600 does not match any spot.
    CHECK(creativesOf(result.matched[0], 0) == vector<unsigned>{ 0 });
    CHECK(creativesOf(result.matched[0], 1).empty());

    // Another country is rejected by the location filter.
    br.location.countryCode = "FR";
    result = pool.filter(br, &conn);
    CHECK(result.matched.empty());
    CHECK(rejectedBy(result, "bidder") == "Location");
}

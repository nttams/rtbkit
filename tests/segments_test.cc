// SegmentList, SegmentsBySource, Id, AccountKey and bid request helpers.

#include "common/account_key.h"
#include "common/bid_request.h"
#include "common/id.h"
#include "common/segments.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <unordered_set>

using namespace RTBKIT;
using nlohmann::json;

TEST_CASE("segments: numeric strings become int segments")
{
    SegmentList list;
    list.add("12");
    list.add("abc");
    list.add("007");      // leading zero: stays a string
    list.add("0");
    list.add("99999999999");  // too big for an int: stays a string
    list.add(5);
    list.sort();

    CHECK(list.ints == std::vector<int>{ 0, 5, 12 });
    CHECK(list.strings == std::vector<std::string>{ "007", "99999999999", "abc" });
    CHECK(list.size() == 6);
    CHECK(!list.empty());
    CHECK(SegmentList().empty());

    CHECK(SegmentList::parseSegmentNum("42") == 42);
    CHECK(SegmentList::parseSegmentNum("-1") == -1);
    CHECK(SegmentList::parseSegmentNum("4x") == -1);
    CHECK(SegmentList::parseSegmentNum("") == -1);
}

TEST_CASE("segments: match and forEach")
{
    SegmentList a, b, c;
    a.add(1); a.add("x");
    b.add(2); b.add("x");
    c.add(3); c.add("y");

    CHECK(a.match(b));    // "x" in common
    CHECK(!a.match(c));
    CHECK(!SegmentList().match(a));

    std::vector<std::pair<int, std::string>> seen;
    a.forEach([&](int i, const std::string& s) { seen.emplace_back(i, s); });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == std::make_pair(1, std::string("1")));
    CHECK(seen[1] == std::make_pair(-1, std::string("x")));
}

TEST_CASE("segments: json")
{
    SegmentList list = SegmentList::fromJson(json::parse(R"([3, "a", ["b", 0.5], 1])"));
    CHECK(list.ints == std::vector<int>{ 3, 1 });
    CHECK(list.strings == std::vector<std::string>{ "a", "b" });
    CHECK(list.toJson() == json::parse(R"([3, 1, "a", "b"])"));

    CHECK_THROWS_AS(SegmentList::fromJson(json::parse("{}")), std::invalid_argument);
    CHECK_THROWS_AS(SegmentList::fromJson(json::parse("[true]")), std::invalid_argument);

    SegmentsBySource segs = SegmentsBySource::fromJson(
            json::parse(R"({"audience": [1, "sports"], "vendor": []})"));
    CHECK(segs.size() == 2);
    CHECK(segs.get("audience").ints == std::vector<int>{ 1 });
    CHECK(segs.get("missing").empty());
    CHECK(segs.toJson() == json::parse(R"({"audience": [1, "sports"], "vendor": []})"));
}

TEST_CASE("segments: by source")
{
    SegmentsBySource segs;
    segs.add("s", 2);
    segs.add("s", 1);
    segs.add("s", "text");
    segs.addInts("i", { 5, 4 });
    segs.addStrings("t", { "b", "a", "7" });
    segs.sortAll();

    CHECK(segs.get("s").ints == std::vector<int>{ 1, 2 });
    CHECK(segs.get("s").strings == std::vector<std::string>{ "text" });
    CHECK(segs.get("i").ints == std::vector<int>{ 4, 5 });
    CHECK(segs.get("t").strings == std::vector<std::string>{ "a", "b" });
    CHECK(segs.get("t").ints == std::vector<int>{ 7 });

    auto shared = std::make_shared<SegmentList>();
    shared->add(9);
    segs.add("s", shared);
    CHECK(segs.get("s").ints == std::vector<int>{ 9 });
}

TEST_CASE("id: numeric and string ids")
{
    CHECK(Id().empty());
    CHECK(!Id());
    CHECK(Id(uint64_t(15)).toString() == "15");
    CHECK(Id(uint64_t(15)) == Id("15"));
    CHECK(Id("a") < Id("b"));
    CHECK(bool(Id("x")));

    std::unordered_set<Id> ids{ Id("a"), Id("b"), Id("a") };
    CHECK(ids.size() == 2);
}

TEST_CASE("account key")
{
    AccountKey key("campaign:strategy");
    REQUIRE(key.size() == 2);
    CHECK(key[0] == "campaign");
    CHECK(key.toString() == "campaign:strategy");
    CHECK(key.toString('/') == "campaign/strategy");
    CHECK(key.parent() == AccountKey{ "campaign" });
    CHECK(key.childKey("x") == AccountKey{ "campaign", "strategy", "x" });
    CHECK(key.hasPrefix(AccountKey{ "campaign" }));
    CHECK(!key.hasPrefix(AccountKey{ "other" }));
    CHECK(key.at(1, "none") == "strategy");
    CHECK(key.at(5, "none") == "none");

    CHECK(AccountKey{ "a", "b" }.hash() == AccountKey("a:b").hash());
    CHECK(AccountKey{ "a", "b" }.hash() != AccountKey{ "ab" }.hash());

    CHECK_THROWS_AS(AccountKey("a::b"), std::invalid_argument);
    CHECK_THROWS_AS(AccountKey("a b"), std::invalid_argument);
    CHECK_THROWS_AS(AccountKey{}.parent(), std::logic_error);

    CHECK(AccountKey::fromJson(json::parse(R"(["hello", "world"])")) == AccountKey("hello:world"));
    CHECK(AccountKey::fromJson(json("hello:world")) == AccountKey{ "hello", "world" });
    CHECK(key.toJson() == json::parse(R"(["campaign", "strategy"])"));
    CHECK_THROWS_AS(AccountKey::fromJson(json(5)), std::invalid_argument);
}

TEST_CASE("bid request helpers")
{
    SECTION("hour of week counts from Sunday midnight UTC")
    {
        using namespace std::chrono;
        // 2024-01-07 was a Sunday, 2024-01-03 a Wednesday.
        sys_seconds sunday = sys_days{ year(2024) / 1 / 7 };
        CHECK(hourOfWeek(sunday) == 0);
        CHECK(hourOfWeek(sunday + hours(5)) == 5);
        CHECK(hourOfWeek(sys_days{ year(2024) / 1 / 3 } + hours(10)) == 3 * 24 + 10);
        CHECK(hourOfWeek(sys_days{ year(2024) / 1 / 6 } + hours(23)) == 167);
    }

    SECTION("url host")
    {
        CHECK(Url("https://User:pw@News.Example.com:8080/a?b#c").host() == "news.example.com");
        CHECK(Url("example.com/path").host() == "example.com");
        CHECK(Url("http://[::1]:80/").host() == "[::1]");
        CHECK(Url().host().empty());
    }

    SECTION("formats")
    {
        CHECK(Format::fromString("300x250") == Format(300, 250));
        CHECK(Format(300, 250).print() == "300x250");
        CHECK(Format::fromJson(json::parse(R"({"width": 1, "height": 2})")) == Format(1, 2));
        CHECK(Format::fromJson(json::parse(R"({"w": 3, "h": 4})")) == Format(3, 4));
        CHECK_THROWS_AS(Format::fromString("300"), std::invalid_argument);
        CHECK_THROWS_AS(Format::fromString("300x250x"), std::invalid_argument);
        CHECK_THROWS_AS(Format::fromJson(json::parse(R"({"width": 1})")), std::invalid_argument);
        CHECK(!Format().valid());
        CHECK(Format(1, 1).valid());
    }

    SECTION("ad positions")
    {
        CHECK(parseAdPosition("above") == AD_POS_ABOVE);
        CHECK(parseAdPosition("BELOW") == AD_POS_BELOW);
        CHECK(parseAdPosition("4") == AD_POS_HEADER);
        CHECK_THROWS_AS(parseAdPosition("middle"), std::invalid_argument);
        CHECK_THROWS_AS(parseAdPosition("42"), std::invalid_argument);
    }

    SECTION("user ids")
    {
        UserIds ids;
        ids.add(Id("e"), ID_EXCHANGE);
        ids.add(Id("p"), ID_PROVIDER);
        ids.add(Id("o"), "other");
        CHECK(ids.exchangeId == Id("e"));
        CHECK(ids.providerId == Id("p"));
        CHECK(ids.size() == 3);
        CHECK_THROWS_AS(ids.add(Id("again"), ID_PROVIDER), std::runtime_error);
    }
}

// Amount and CurrencyCode.

#include "common/currency.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sstream>
#include <unordered_map>

using namespace RTBKIT;

TEST_CASE("amount: construction")
{
    Amount none;
    CHECK(none.isZero());
    CHECK(none.currencyCode == CurrencyCode::None);

    Amount usd(CurrencyCode::USD, 1500);
    CHECK(usd.value == 1500);
    CHECK(usd.isNonNegative());
    CHECK(!usd.isNegative());
    CHECK(bool(usd));

    CHECK(Amount("USD/1M", 7).currencyCode == CurrencyCode::USD);
    CHECK(Amount("EUR", 7).currencyCode == CurrencyCode::EUR);
    CHECK_THROWS_AS(Amount("GBP", 1), std::invalid_argument);

    // No currency means no value.
    CHECK_THROWS_AS(Amount(CurrencyCode::None, 5), std::invalid_argument);
    CHECK(Amount(CurrencyCode::None, 0).isZero());
}

TEST_CASE("amount: arithmetic")
{
    Amount a(CurrencyCode::USD, 1000);
    Amount b(CurrencyCode::USD, 250);

    CHECK((a + b).value == 1250);
    CHECK((a - b).value == 750);
    CHECK((b - a).value == -750);
    CHECK((b - a).isNegative());
    CHECK((-a).value == -1000);
    CHECK((a * 1.5).value == 1500);
    CHECK((a * 0.0001).value == 0);
    CHECK((Amount(CurrencyCode::USD, -999) * 0.5).value == -499);  // truncates

    Amount c = a;
    c += b;
    c -= Amount(CurrencyCode::USD, 50);
    CHECK(c == Amount(CurrencyCode::USD, 1200));

    // Zero adopts the other currency.
    Amount zero;
    zero += a;
    CHECK(zero == a);
    CHECK((Amount() - a) == -a);
    CHECK((a + Amount()) == a);

    // Mixed currencies don't add.
    Amount eur(CurrencyCode::EUR, 10);
    CHECK_THROWS_AS(a + eur, std::invalid_argument);
    CHECK_THROWS_AS(a - eur, std::invalid_argument);
}

TEST_CASE("amount: comparison")
{
    Amount lo(CurrencyCode::USD, 100);
    Amount hi(CurrencyCode::USD, 200);

    CHECK(lo < hi);
    CHECK(lo <= hi);
    CHECK(hi > lo);
    CHECK(hi >= lo);
    CHECK(lo <= lo);
    CHECK(lo != hi);
    CHECK(lo == Amount(CurrencyCode::USD, 100));
    CHECK(lo.limit(hi) == lo);
    CHECK(hi.limit(lo) == lo);

    // Zero amounts are equal whatever their currency.
    CHECK(Amount() == Amount(CurrencyCode::EUR, 0));
    CHECK(Amount(CurrencyCode::USD, 0) == Amount(CurrencyCode::EUR, 0));
    CHECK(Amount() < lo);

    // Different currencies are equal never, ordered never.
    Amount eur(CurrencyCode::EUR, 100);
    CHECK(lo != eur);
    CHECK_THROWS_AS(lo < eur, std::invalid_argument);
    CHECK_THROWS_AS(lo.limit(eur), std::invalid_argument);
}

TEST_CASE("amount: micro units, CPM and natural units")
{
    // 1 USD is a million micro units, a $1 CPM is 1000 micros per impression.
    CHECK(Amount::fromUnits(CurrencyCode::USD, 1.0).value == 1'000'000);
    CHECK(Amount::fromUnits(CurrencyCode::USD, 0.000123).value == 123);
    CHECK(Amount::fromCpm(CurrencyCode::USD, 1.0).value == 1000);
    CHECK(USD_CPM(2.5).value == 2500);
    CHECK(EUR_CPM(2.5) == Amount(CurrencyCode::EUR, 2500));
    CHECK(MicroUSD(42).value == 42);

    CHECK(USD_CPM(2.5).toCpm() == 2.5);
    CHECK(Amount(CurrencyCode::USD, 2'500'000).toUnits() == 2.5);

    // Fixed point: no floating point drift when accumulating micro units.
    Amount sum;
    for (int i = 0; i < 1000; ++i) sum += MicroUSD(1);
    CHECK(sum.value == 1000);
}

TEST_CASE("amount: text form")
{
    CHECK(Amount().toString() == "0");
    CHECK(Amount(CurrencyCode::USD, 1500).toString() == "1500USD/1M");
    CHECK(Amount(CurrencyCode::EUR, -3).toString() == "-3EUR/1M");
    CHECK(Amount(CurrencyCode::IMP, 4).toString() == "4IMP");

    CHECK(Amount::parse("0") == Amount());
    CHECK(Amount::parse("1500USD/1M") == Amount(CurrencyCode::USD, 1500));
    CHECK(Amount::parse("-3EUR/1M") == Amount(CurrencyCode::EUR, -3));
    CHECK_THROWS_AS(Amount::parse("15"), std::invalid_argument);
    CHECK_THROWS_AS(Amount::parse("USD"), std::invalid_argument);
    CHECK_THROWS_AS(Amount::parse("15XYZ"), std::invalid_argument);

    std::ostringstream out;
    out << Amount(CurrencyCode::USD, 9);
    CHECK(out.str() == "9USD/1M");

    CHECK(toString(CurrencyCode::USD) == "USD");
    CHECK(parseCurrencyCode("USD") == CurrencyCode::USD);
    CHECK(parseCurrencyCode("EUR/1M") == CurrencyCode::EUR);
    CHECK_THROWS_AS(parseCurrencyCode("usd"), std::invalid_argument);
}

TEST_CASE("amount: json")
{
    using nlohmann::json;

    json j = Amount(CurrencyCode::USD, 1500);
    CHECK(j == json::array({ 1500, "USD/1M" }));
    CHECK(json(Amount()) == 0);

    CHECK(j.get<Amount>() == Amount(CurrencyCode::USD, 1500));
    CHECK(json(0).get<Amount>() == Amount());
    CHECK(json(nullptr).get<Amount>() == Amount());
    CHECK(json("12EUR/1M").get<Amount>() == Amount(CurrencyCode::EUR, 12));
    CHECK(json::parse(R"({"value": 5, "currencyCode": "USD/1M"})").get<Amount>()
          == Amount(CurrencyCode::USD, 5));

    CHECK_THROWS_AS(json::parse("[1]").get<Amount>(), std::invalid_argument);
    CHECK_THROWS_AS(json::parse("true").get<Amount>(), std::invalid_argument);
    CHECK_THROWS_AS(json::parse(R"({"nope": 1})").get<Amount>(), std::invalid_argument);
}

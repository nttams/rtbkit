#include "common/currency.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <ostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace RTBKIT {

namespace {

std::string longName(CurrencyCode code)
{
    switch (code) {
    case CurrencyCode::None: return "NONE";
    case CurrencyCode::USD:  return "USD/1M";
    case CurrencyCode::EUR:  return "EUR/1M";
    case CurrencyCode::IMP:  return "IMP";
    case CurrencyCode::CLK:  return "CLK";
    }
    throw std::invalid_argument("unknown currency code");
}

} // namespace

std::string toString(CurrencyCode code)
{
    switch (code) {
    case CurrencyCode::None: return "NONE";
    case CurrencyCode::USD:  return "USD";
    case CurrencyCode::EUR:  return "EUR";
    case CurrencyCode::IMP:  return "IMP";
    case CurrencyCode::CLK:  return "CLK";
    }
    throw std::invalid_argument("unknown currency code");
}

CurrencyCode parseCurrencyCode(const std::string& code)
{
    for (CurrencyCode c : { CurrencyCode::None, CurrencyCode::USD,
                            CurrencyCode::EUR, CurrencyCode::IMP,
                            CurrencyCode::CLK }) {
        if (code == toString(c) || code == longName(c)) return c;
    }
    throw std::invalid_argument("unknown currency code " + code);
}

Amount::Amount(CurrencyCode code, int64_t v) : value(v), currencyCode(code)
{
    if (code == CurrencyCode::None && v != 0)
        throw std::invalid_argument("an amount without currency must be zero");
}

Amount::Amount(const std::string& currency, int64_t v)
    : Amount(parseCurrencyCode(currency), v)
{
}

Amount Amount::limit(const Amount& other) const
{
    if (!currencyIsCompatible(other))
        throw std::invalid_argument("currencies are not compatible: "
                                    + toString() + " vs " + other.toString());
    return Amount(currencyCode == CurrencyCode::None ? other.currencyCode
                                                     : currencyCode,
                  std::min(value, other.value));
}

Amount& Amount::operator+=(const Amount& other)
{
    if (other.isZero()) return *this;
    if (isZero()) return *this = other;
    if (currencyCode != other.currencyCode)
        throw std::invalid_argument("cannot add " + other.toString()
                                    + " to " + toString());
    value += other.value;
    return *this;
}

Amount& Amount::operator-=(const Amount& other)
{
    if (other.isZero()) return *this;
    if (isZero()) return *this = -other;
    if (currencyCode != other.currencyCode)
        throw std::invalid_argument("cannot subtract " + other.toString()
                                    + " from " + toString());
    value -= other.value;
    return *this;
}

Amount Amount::operator+(const Amount& other) const
{
    Amount result = *this;
    result += other;
    return result;
}

Amount Amount::operator-(const Amount& other) const
{
    Amount result = *this;
    result -= other;
    return result;
}

Amount Amount::operator-() const
{
    Amount result = *this;
    result.value = -result.value;
    return result;
}

Amount Amount::operator*(double factor) const
{
    return Amount(currencyCode, static_cast<int64_t>(double(value) * factor));
}

bool Amount::currencyIsCompatible(const Amount& other) const
{
    return currencyCode == other.currencyCode
        || (currencyCode == CurrencyCode::None && value == 0)
        || (other.currencyCode == CurrencyCode::None && other.value == 0);
}

bool Amount::operator==(const Amount& other) const
{
    return value == other.value
        && (value == 0 || currencyCode == other.currencyCode);
}

#define RTB_AMOUNT_CMP(_op_)                                                \
    bool Amount::operator _op_(const Amount& other) const                   \
    {                                                                       \
        if (!currencyIsCompatible(other))                                   \
            throw std::invalid_argument("currencies are not compatible: "   \
                                        + toString() + " vs "               \
                                        + other.toString());                \
        return value _op_ other.value;                                      \
    }

RTB_AMOUNT_CMP(<)
RTB_AMOUNT_CMP(<=)
RTB_AMOUNT_CMP(>)
RTB_AMOUNT_CMP(>=)

#undef RTB_AMOUNT_CMP

std::string Amount::currencyStr() const { return longName(currencyCode); }

std::string Amount::toString() const
{
    if (isZero()) return "0";
    return std::to_string(value) + currencyStr();
}

Amount Amount::parse(const std::string& text)
{
    if (text == "0") return Amount();

    const char* p = text.c_str();
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(p, &end, 10);
    if (errno != 0 || end == p)
        throw std::invalid_argument("invalid amount: " + text);

    std::string currency(end);
    if (currency.empty())
        throw std::invalid_argument("no currency on amount " + text);

    return Amount(currency, v);
}

Amount Amount::fromCpm(CurrencyCode code, double cpm)
{
    return Amount(code, static_cast<int64_t>(std::llround(cpm * 1000.0)));
}

Amount Amount::fromUnits(CurrencyCode code, double units)
{
    return Amount(code, static_cast<int64_t>(std::llround(units * 1'000'000.0)));
}

std::ostream& operator<<(std::ostream& stream, const Amount& amount)
{
    return stream << amount.toString();
}

void to_json(nlohmann::json& j, const Amount& amount)
{
    if (amount.isZero()) j = 0;
    else j = nlohmann::json::array({ amount.value, amount.currencyStr() });
}

void from_json(const nlohmann::json& j, Amount& amount)
{
    if (j.is_array()) {
        if (j.size() != 2) throw std::invalid_argument("invalid amount size");
        amount = Amount(j[1].get<std::string>(), j[0].get<int64_t>());
    }
    else if (j.is_string()) amount = Amount::parse(j.get<std::string>());
    else if (j.is_null()) amount = Amount();
    else if (j.is_number() && j.get<double>() == 0.0) amount = Amount();
    else if (j.is_object()) {
        std::string code = "NONE";
        int64_t value = 0;
        for (const auto& [key, val] : j.items()) {
            if (key == "value") value = val.get<int64_t>();
            else if (key == "currencyCode") code = val.get<std::string>();
            else throw std::invalid_argument("unknown Amount field " + key);
        }
        amount = Amount(code, value);
    }
    else throw std::invalid_argument("unknown amount " + j.dump());
}

} // namespace RTBKIT

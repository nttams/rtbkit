// Fixed point money, like rtbkit/common/currency.h. Values are integers in
// micro-units of the currency (1 USD == 1'000'000). An Amount in the
// NONE currency is always zero.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace RTBKIT {

enum class CurrencyCode : uint32_t {
    None = 'N' << 24 | 'O' << 16 | 'N' << 8 | 'E',
    USD  = 'U' << 24 | 'S' << 16 | 'D' << 8,
    EUR  = 'E' << 24 | 'U' << 16 | 'R' << 8,
    IMP  = 'I' << 24 | 'M' << 16 | 'P' << 8,
    CLK  = 'C' << 24 | 'L' << 16 | 'K' << 8
};

/** Short name: "USD", "EUR", "IMP", "CLK", "NONE". */
std::string toString(CurrencyCode code);

/** Accepts the short name or the long form ("USD/1M"). Case sensitive.
    Throws std::invalid_argument for anything else. */
CurrencyCode parseCurrencyCode(const std::string& code);

struct Amount {
    Amount() = default;

    /** Throws std::invalid_argument if code is None and value is not 0. */
    Amount(CurrencyCode code, int64_t value);
    Amount(const std::string& currency, int64_t value);

    bool isZero() const { return value == 0; }
    bool isNegative() const { return value < 0; }
    bool isNonNegative() const { return value >= 0; }
    explicit operator bool() const { return value != 0; }

    /** The smaller of the two amounts. */
    Amount limit(const Amount& other) const;

    // Adding to or subtracting from a zero amount adopts the other currency.
    // Mixing two non-zero currencies throws std::invalid_argument.
    Amount& operator+=(const Amount& other);
    Amount& operator-=(const Amount& other);
    Amount operator+(const Amount& other) const;
    Amount operator-(const Amount& other) const;
    Amount operator-() const;

    /** Scales the value, truncating towards zero. */
    Amount operator*(double factor) const;

    /** Equal if value and currency match, or both are zero. */
    bool operator==(const Amount& other) const;
    bool operator!=(const Amount& other) const { return !(*this == other); }

    // Ordering needs compatible currencies, otherwise std::invalid_argument.
    bool operator<(const Amount& other) const;
    bool operator<=(const Amount& other) const;
    bool operator>(const Amount& other) const;
    bool operator>=(const Amount& other) const;

    bool currencyIsCompatible(const Amount& other) const;

    /** "1500USD/1M", or "0" for a zero amount. */
    std::string toString() const;
    static Amount parse(const std::string& text);

    /** CPM amount in natural units: cpm 1.5 USD == 1500 micro USD per
        impression. */
    static Amount fromCpm(CurrencyCode code, double cpm);
    /** Natural units: 1.0 USD == 1'000'000. */
    static Amount fromUnits(CurrencyCode code, double units);

    double toCpm() const { return double(value) / 1000.0; }
    double toUnits() const { return double(value) / 1'000'000.0; }

    /** "USD/1M" style name of the currency. */
    std::string currencyStr() const;

    int64_t value = 0;
    CurrencyCode currencyCode = CurrencyCode::None;
};

inline Amount USD_CPM(double cpm) { return Amount::fromCpm(CurrencyCode::USD, cpm); }
inline Amount EUR_CPM(double cpm) { return Amount::fromCpm(CurrencyCode::EUR, cpm); }
inline Amount MicroUSD(int64_t micros) { return Amount(CurrencyCode::USD, micros); }

std::ostream& operator<<(std::ostream& stream, const Amount& amount);

/** JSON form is [value, "USD/1M"], or 0 for a zero amount. Parsing also
    accepts "1500USD/1M", null and {"value":..,"currencyCode":..}. */
void to_json(nlohmann::json& j, const Amount& amount);
void from_json(const nlohmann::json& j, Amount& amount);

} // namespace RTBKIT

// Hierarchical account name, e.g. {"campaign", "strategy"} <-> "campaign:strategy".
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace RTBKIT {

/** Slugs are non-empty, at most 256 chars of letters, digits, '_' and '.'.
    Throws std::invalid_argument otherwise. */
void validateSlug(const std::string& slug);

struct AccountKey : public std::vector<std::string> {
    AccountKey() = default;

    /** Splits on the delimiter and validates every part. */
    AccountKey(const std::string& str, char delimiter = ':');
    AccountKey(const char* str) : AccountKey(std::string(str)) {}
    AccountKey(const std::vector<std::string>& parts);
    AccountKey(std::initializer_list<std::string> parts);

    std::string toString(char delimiter = ':') const;

    /** Throws std::logic_error on an empty key. */
    AccountKey parent() const;
    AccountKey childKey(const std::string& childName) const;
    bool hasPrefix(const AccountKey& prefix) const;

    uint64_t hash() const;

    /** Part at index, or fallback if the key is shorter. */
    std::string at(unsigned index, const std::string& fallback) const;
    using std::vector<std::string>::at;

    /** Accepts a JSON array of slugs or a "a:b" string. */
    static AccountKey fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;

private:
    void validate() const;
};

std::ostream& operator<<(std::ostream& stream, const AccountKey& key);

} // namespace RTBKIT

template<>
struct std::hash<RTBKIT::AccountKey> {
    size_t operator()(const RTBKIT::AccountKey& key) const noexcept
    {
        return size_t(key.hash());
    }
};

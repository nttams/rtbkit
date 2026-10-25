#include "common/account_key.h"

#include <algorithm>
#include <cctype>
#include <ostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace RTBKIT {

void validateSlug(const std::string& slug)
{
    if (slug.empty())
        throw std::invalid_argument("account slug cannot be empty");
    if (slug.size() > 256)
        throw std::invalid_argument("account slug has max length of 256");

    for (char c : slug) {
        if (c != '_' && c != '.' && !std::isalnum(static_cast<unsigned char>(c)))
            throw std::invalid_argument("account slug has invalid character '"
                                        + std::string(1, c) + "': " + slug);
    }
}

namespace {

std::vector<std::string> split(const std::string& str, char delimiter)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = str.find(delimiter, start);
        parts.push_back(str.substr(start, pos == std::string::npos
                                              ? pos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

} // namespace

AccountKey::AccountKey(const std::string& str, char delimiter)
    : std::vector<std::string>(split(str, delimiter))
{
    validate();
}

AccountKey::AccountKey(const std::vector<std::string>& parts)
    : std::vector<std::string>(parts)
{
    validate();
}

AccountKey::AccountKey(std::initializer_list<std::string> parts)
    : std::vector<std::string>(parts)
{
    validate();
}

void AccountKey::validate() const
{
    for (const auto& slug : *this) validateSlug(slug);
}

std::string AccountKey::toString(char delimiter) const
{
    std::string result;
    for (size_t i = 0; i < size(); ++i) {
        if (i != 0) result += delimiter;
        result += (*this)[i];
    }
    return result;
}

AccountKey AccountKey::parent() const
{
    if (empty()) throw std::logic_error("account key has no parent");
    AccountKey result = *this;
    result.pop_back();
    return result;
}

AccountKey AccountKey::childKey(const std::string& childName) const
{
    validateSlug(childName);
    AccountKey result = *this;
    result.push_back(childName);
    return result;
}

bool AccountKey::hasPrefix(const AccountKey& prefix) const
{
    return size() >= prefix.size()
        && std::equal(prefix.begin(), prefix.end(), begin());
}

uint64_t AccountKey::hash() const
{
    // FNV-1a over the parts, with a separator so {"a","b"} != {"ab"}.
    uint64_t h = 1469598103934665603ULL;
    for (const auto& part : *this) {
        for (unsigned char c : part) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0xff; h *= 1099511628211ULL;
    }
    return h;
}

std::string AccountKey::at(unsigned index, const std::string& fallback) const
{
    return index < size() ? (*this)[index] : fallback;
}

AccountKey AccountKey::fromJson(const nlohmann::json& j)
{
    if (j.is_string()) return AccountKey(j.get<std::string>());
    if (j.is_array()) return AccountKey(j.get<std::vector<std::string>>());
    throw std::invalid_argument("account must be a string or an array of strings");
}

nlohmann::json AccountKey::toJson() const
{
    return static_cast<const std::vector<std::string>&>(*this);
}

std::ostream& operator<<(std::ostream& stream, const AccountKey& key)
{
    return stream << key.toString();
}

} // namespace RTBKIT

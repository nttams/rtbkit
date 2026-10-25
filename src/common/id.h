// Opaque identifier. Numeric ids are stored in their decimal string form so
// that every id has one canonical representation.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace RTBKIT {

struct Id {
    Id() = default;
    explicit Id(uint64_t value) : str_(std::to_string(value)) {}
    explicit Id(std::string value) : str_(std::move(value)) {}

    const std::string& toString() const { return str_; }
    bool empty() const { return str_.empty(); }
    explicit operator bool() const { return !str_.empty(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

private:
    std::string str_;
};

} // namespace RTBKIT

template<>
struct std::hash<RTBKIT::Id> {
    size_t operator()(const RTBKIT::Id& id) const noexcept
    {
        return std::hash<std::string>()(id.toString());
    }
};

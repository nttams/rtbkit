// Port of rtbkit/common/segments.h. A segment is either a non-negative int
// or a string; a SegmentList keeps both kinds apart.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace RTBKIT {

struct SegmentList {
    void add(int i) { ints.push_back(i); }

    /** Numeric strings ("12") become int segments. add() does not sort, call
        sort() when done. */
    void add(const std::string& str);
    void add(const char* str) { add(std::string(str)); }

    void sort();

    size_t size() const { return ints.size() + strings.size(); }
    bool empty() const { return ints.empty() && strings.empty(); }

    /** Returns the int value of a canonical non-negative decimal string, else
        -1 (so "007", "-1", "1a" and values over INT_MAX stay strings). */
    static int parseSegmentNum(const std::string& str);

    /** True if the two lists have at least one segment in common. */
    bool match(const SegmentList& other) const;

    void forEach(const std::function<void(int, const std::string&)>& onSegment) const;

    /** From a JSON array of ints and strings. A [value, weight] pair is
        accepted and the weight is ignored. */
    static SegmentList fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;

    bool operator==(const SegmentList&) const = default;

    std::vector<int> ints;
    std::vector<std::string> strings;
};

/** Segments grouped by their source ("audience", "iab-categories", ...). */
struct SegmentsBySource
    : public std::map<std::string, std::shared_ptr<SegmentList>> {

    /** Returns the list for the source, or an empty list. */
    const SegmentList& get(const std::string& source) const;

    void sortAll();

    void add(const std::string& source, const std::shared_ptr<SegmentList>& segs);
    void addInts(const std::string& source, const std::vector<int>& segs);
    void addStrings(const std::string& source, const std::vector<std::string>& segs);

    /** Adds one segment to the source, creating the source if needed. */
    void add(const std::string& source, const std::string& segment);
    void add(const std::string& source, int segment);

    static SegmentsBySource fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

} // namespace RTBKIT

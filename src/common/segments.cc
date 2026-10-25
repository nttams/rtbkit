#include "common/segments.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace RTBKIT {

void SegmentList::add(const std::string& str)
{
    int i = parseSegmentNum(str);
    if (i == -1) strings.push_back(str);
    else add(i);
}

void SegmentList::sort()
{
    std::sort(ints.begin(), ints.end());
    std::sort(strings.begin(), strings.end());
}

int SegmentList::parseSegmentNum(const std::string& str)
{
    if (str.empty()) return -1;
    if (str == "0") return 0;
    if (str[0] < '1' || str[0] > '9') return -1;

    for (char c : str)
        if (c < '0' || c > '9') return -1;

    char* end = nullptr;
    long long i = std::strtoll(str.c_str(), &end, 10);
    if (i > INT_MAX || i < 0 || end != str.c_str() + str.size()) return -1;
    return int(i);
}

bool SegmentList::match(const SegmentList& other) const
{
    for (int i : ints)
        if (std::find(other.ints.begin(), other.ints.end(), i) != other.ints.end())
            return true;
    for (const auto& s : strings)
        if (std::find(other.strings.begin(), other.strings.end(), s)
            != other.strings.end())
            return true;
    return false;
}

void SegmentList::forEach(
        const std::function<void(int, const std::string&)>& onSegment) const
{
    for (int i : ints) onSegment(i, std::to_string(i));
    for (const auto& s : strings) onSegment(-1, s);
}

SegmentList SegmentList::fromJson(const nlohmann::json& j)
{
    if (!j.is_array())
        throw std::invalid_argument("segment list must be an array");

    SegmentList result;
    for (const auto& entry : j) {
        const nlohmann::json* value = &entry;
        if (entry.is_array()) {
            if (entry.empty() || entry.size() > 2)
                throw std::invalid_argument("bad weighted segment");
            value = &entry[0];
        }

        if (value->is_string()) result.add(value->get<std::string>());
        else if (value->is_number_integer()) {
            int64_t v = value->get<int64_t>();
            if (v >= 0 && v <= INT_MAX) result.add(int(v));
            else result.strings.push_back(std::to_string(v));
        }
        else throw std::invalid_argument("segment must be an int or a string: "
                                         + value->dump());
    }
    return result;
}

nlohmann::json SegmentList::toJson() const
{
    nlohmann::json result = nlohmann::json::array();
    for (int i : ints) result.push_back(i);
    for (const auto& s : strings) result.push_back(s);
    return result;
}

const SegmentList& SegmentsBySource::get(const std::string& source) const
{
    static const SegmentList empty;
    auto it = find(source);
    return it == end() ? empty : *it->second;
}

void SegmentsBySource::sortAll()
{
    for (auto& entry : *this) entry.second->sort();
}

void SegmentsBySource::add(const std::string& source,
                           const std::shared_ptr<SegmentList>& segs)
{
    (*this)[source] = segs;
}

void SegmentsBySource::addInts(const std::string& source, const std::vector<int>& segs)
{
    for (int i : segs) add(source, i);
}

void SegmentsBySource::addStrings(const std::string& source,
                                  const std::vector<std::string>& segs)
{
    for (const auto& s : segs) add(source, s);
}

void SegmentsBySource::add(const std::string& source, const std::string& segment)
{
    auto& list = (*this)[source];
    if (!list) list = std::make_shared<SegmentList>();
    list->add(segment);
}

void SegmentsBySource::add(const std::string& source, int segment)
{
    auto& list = (*this)[source];
    if (!list) list = std::make_shared<SegmentList>();
    list->add(segment);
}

SegmentsBySource SegmentsBySource::fromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("segments must be an object of source to list");

    SegmentsBySource result;
    for (const auto& [source, list] : j.items())
        result[source] = std::make_shared<SegmentList>(SegmentList::fromJson(list));
    return result;
}

nlohmann::json SegmentsBySource::toJson() const
{
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [source, list] : *this) result[source] = list->toJson();
    return result;
}

} // namespace RTBKIT

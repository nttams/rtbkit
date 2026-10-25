// Port of rtbkit/common/segments.h (only what the filters use).
#pragma once

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace RTBKIT {

struct SegmentList {
    SegmentList() = default;

    void add(int i) { ints.push_back(i); }

    // add(...) does not sort the segments. Should be done manually.
    void add(const std::string& str)
    {
        int i = parseSegmentNum(str);
        if (i == -1) strings.push_back(str);
        else add(i);
    }

    void add(const char* str) { add(std::string(str)); }

    void sort()
    {
        std::sort(ints.begin(), ints.end());
        std::sort(strings.begin(), strings.end());
    }

    size_t size() const { return ints.size() + strings.size(); }
    bool empty() const { return ints.empty() && strings.empty(); }

    static int parseSegmentNum(const std::string& str)
    {
        if (str.empty()) return -1;
        if (str.length() == 1 && str[0] == '0') return 0;
        if (str[0] != '0' && isdigit(static_cast<unsigned char>(str[0]))) {
            char* endptr = nullptr;
            long i = strtol(str.c_str(), &endptr, 10);
            if (endptr == str.c_str() + str.length()) {
                if (i < 0) return -1;
                return int(i);
            }
        }
        return -1;
    }

    /** Iterate over all segments and call the given callback. */
    void forEach(const std::function<void (int, std::string)>& onSegment) const
    {
        for (int i : ints) onSegment(i, std::to_string(i));
        for (const auto& s : strings) onSegment(-1, s);
    }

    std::vector<int> ints;
    std::vector<std::string> strings;
};

typedef std::map<std::string, std::shared_ptr<SegmentList> > SegmentsBySource;

} // namespace RTBKIT

// Port of rtbkit/common/filter.h: ConfigSet, CreativeMatrix, FilterState and
// FilterBase.
#pragma once

#include "common/bid_request.h"
#include "common/check.h"
#include "common/exchange_filter_hooks.h"

#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTBKIT {

struct AgentConfig;

/******************************************************************************/
/* CONFIG SET                                                                 */
/******************************************************************************/

/** Represents a set of config ids as a bitfield to enable efficient batch
    processing of configs during filtering. A value of 1 at position i in the
    bitfield indicates the config id i is part of the set.

    The config set is dynamically expanded as new config ids are added. When
    expanding it uses the defaultValue provided to the constructor to determine
    whether configs should be part of the set by default or not. In other words
    ConfigSet(true) indicates that all configs are part of the set by default.
 */
struct ConfigSet
{
    typedef uint64_t Word;
    static constexpr size_t Div = sizeof(Word) * 8;

    explicit ConfigSet(bool defaultValue = false) :
        defaultValue(defaultValue ? ~Word(0) : 0)
    {}

    size_t size() const { return bitfield.size() * Div; }

    // Expands the set to contain at least newSize configs and use the
    // defaultValue provided to the constructor to initialize the bit field.
    void expand(size_t newSize)
    {
        if (newSize) newSize = (newSize - 1) / Div + 1; // ceilDiv(newSize, Div)
        if (newSize <= bitfield.size()) return;
        bitfield.resize(newSize, defaultValue);
    }

    void set(size_t index)
    {
        expand(index + 1);
        bitfield[index / Div] |= 1ULL << (index % Div);
    }

    void set(size_t index, bool value)
    {
        if (value) set(index);
        else reset(index);
    }

    void reset(size_t index)
    {
        expand(index + 1);
        bitfield[index / Div] &= ~(1ULL << (index % Div));
    }

    bool operator[] (size_t index) const { return test(index); }

    bool test(size_t index) const
    {
        if (index >= size()) return defaultValue;
        return bitfield[index / Div] & (1ULL << (index % Div));
    }

    size_t count() const
    {
        size_t total = 0;
        for (Word w : bitfield) total += std::popcount(w);
        return total;
    }

    bool empty() const
    {
        if (bitfield.empty()) return !defaultValue;

        for (Word w : bitfield) {
            if (w) return false;
        }
        return true;
    }

#define RTBKIT_CONFIG_SET_OP(_op_)                                      \
    ConfigSet& operator _op_ (const ConfigSet& other)                   \
    {                                                                   \
        expand(other.size());                                           \
                                                                        \
        for (size_t i = 0; i < other.bitfield.size(); ++i)              \
            bitfield[i] _op_ other.bitfield[i];                         \
                                                                        \
        for (size_t i = other.bitfield.size(); i < bitfield.size(); ++i) \
            bitfield[i] _op_ other.defaultValue;                        \
                                                                        \
        return *this;                                                   \
    }

    RTBKIT_CONFIG_SET_OP(&=)
    RTBKIT_CONFIG_SET_OP(|=)
    RTBKIT_CONFIG_SET_OP(^=)

#undef RTBKIT_CONFIG_SET_OP

#define RTBKIT_CONFIG_SET_OP_CONST(_op_)                        \
    ConfigSet operator _op_ (const ConfigSet& other) const      \
    {                                                           \
        ConfigSet tmp = *this;                                  \
        tmp _op_ ## = other;                                    \
        return tmp;                                             \
    }

    RTBKIT_CONFIG_SET_OP_CONST(&)
    RTBKIT_CONFIG_SET_OP_CONST(|)
    RTBKIT_CONFIG_SET_OP_CONST(^)

#undef RTBKIT_CONFIG_SET_OP_CONST

    // The not(~) operator which doesn't have a analogue in set terminology.
    ConfigSet& negate()
    {
        defaultValue = ~defaultValue;
        for (Word& w : bitfield) w = ~w;
        return *this;
    }

    ConfigSet negate() const
    {
        return ConfigSet(*this).negate();
    }

    /** Utility to iterate over every config ids that is part of the bitfield.
        Usage example:

        for (size_t id = set.next(); id < set.size(); id = set.next(id + 1)) {
            // ...
        }
     */
    size_t next(size_t start = 0) const
    {
        size_t topIndex = start / Div;
        size_t subIndex = start % Div;
        Word mask = ~Word(0) & ~((Word(1) << subIndex) - 1);

        for (size_t i = topIndex; i < bitfield.size(); ++i) {
            Word value = bitfield[i] & mask;
            mask = ~Word(0);

            if (!value) continue;

            return (i * Div) + std::countr_zero(value);
        }

        return size();
    }

    std::string print() const
    {
        std::stringstream ss;
        ss << "{ " << std::hex;
        for (Word w : bitfield) ss << w << " ";
        ss << "d:" << (defaultValue ? "1" : "0") << " ";
        ss << "}";
        return ss.str();
    }

private:
    std::vector<Word> bitfield;
    Word defaultValue;
};


/******************************************************************************/
/* CREATIVE MATRIX                                                            */
/******************************************************************************/

/** Two dimensional bitfield of (creative index, config index): a 1 at
    [creative][config] means that creative of that config is part of the set.
    Like ConfigSet it grows on demand, new rows get the default value. */
struct CreativeMatrix
{
    explicit CreativeMatrix(bool defaultValue = false) :
        defaultValue(ConfigSet(defaultValue))
    {}

    explicit CreativeMatrix(ConfigSet defaultValue) :
        defaultValue(std::move(defaultValue))
    {}

    size_t size() const { return matrix.size(); }

    bool empty() const
    {
        for (const ConfigSet& set : matrix) {
            if (!set.empty()) return false;
        }
        return true;
    }

    void expand(size_t newSize)
    {
        if (newSize <= matrix.size()) return;
        matrix.resize(newSize, defaultValue);
    }

    const ConfigSet& operator[] (size_t creative) const
    {
        return creative < matrix.size() ? matrix[creative] : defaultValue;
    }

    bool test(size_t creative, size_t config) const
    {
        return (*this)[creative].test(config);
    }

    void set(size_t creative, size_t config, bool value = true)
    {
        expand(creative + 1);
        matrix[creative].set(config, value);
    }

    // Sets the first numCreatives creatives of the given config.
    void setConfig(size_t config, size_t numCreatives)
    {
        expand(numCreatives);
        for (size_t cr = 0; cr < numCreatives; ++cr)
            matrix[cr].set(config);
    }

    void reset(size_t creative, size_t config)
    {
        expand(creative + 1);
        matrix[creative].reset(config);
    }

    void resetConfig(size_t config)
    {
        for (ConfigSet& set : matrix) set.reset(config);
    }

#define RTBKIT_CREATIVE_MATRIX_OP(_op_)                                 \
    CreativeMatrix& operator _op_ (const CreativeMatrix& other)         \
    {                                                                   \
        expand(other.matrix.size());                                    \
                                                                        \
        for (size_t i = 0; i < other.matrix.size(); ++i)                \
            matrix[i] _op_ other.matrix[i];                             \
                                                                        \
        for (size_t i = other.matrix.size(); i < matrix.size(); ++i)    \
            matrix[i] _op_ other.defaultValue;                          \
                                                                        \
        return *this;                                                   \
    }

    RTBKIT_CREATIVE_MATRIX_OP(&=)
    RTBKIT_CREATIVE_MATRIX_OP(|=)
    RTBKIT_CREATIVE_MATRIX_OP(^=)

#undef RTBKIT_CREATIVE_MATRIX_OP

    // The bit-wise not(~) operator.
    CreativeMatrix& negate()
    {
        defaultValue.negate();
        for (ConfigSet& set : matrix) set.negate();
        return *this;
    }

    CreativeMatrix negate() const
    {
        return CreativeMatrix(*this).negate();
    }

    // Returns a ConfigSet where a config will be present iff there's at least
    // one creative present for that config in the matrix.
    ConfigSet aggregate() const
    {
        ConfigSet configs;
        for (const ConfigSet& set : matrix) configs |= set;
        return configs;
    }

    std::string print() const
    {
        std::stringstream ss;

        ss << "[ ";
        for (size_t cr = 0; cr < matrix.size(); ++cr)
            ss << cr << ":" << matrix[cr].print() << " ";
        ss << "d:" << defaultValue.print() << " ";
        ss << "]";

        return ss.str();
    }

private:
    std::vector<ConfigSet> matrix;
    ConfigSet defaultValue;
};


/******************************************************************************/
/* BIDDABLE SPOTS                                                             */
/******************************************************************************/

/** An ad spot of the bid request and the creatives that may be used on it. */
struct BiddableSpot
{
    unsigned spot;                  ///< Index in BidRequest::imp
    std::vector<unsigned> creatives; ///< Indexes in AgentConfig::creatives
};

/** All the spots a config can bid on, ordered by spot index. */
typedef std::vector<BiddableSpot> BiddableSpots;


/******************************************************************************/
/* FILTER STATE                                                               */
/******************************************************************************/

/** Contains the current state of the filtering process for a single bid
    request. It's also used to extract information after the filtering is
    over.

    The state includes a CreativeMatrix for each impression in the bid request
    so that we can track which creative is available for bidding on a per
    impression basis. */
struct FilterState
{
    FilterState(
            const BidRequest& br,
            const ExchangeFilterHooks* ex,
            const CreativeMatrix& activeConfigs) :
        request(br),
        exchange(ex),
        configs_(activeConfigs.aggregate()),
        creatives_(br.imp.size(), activeConfigs)
    {}

    const BidRequest& request;
    const ExchangeFilterHooks * const exchange;

    // Current set of active configuration.
    const ConfigSet& configs() const { return configs_; }

    // Restrict the number of active configs to those specified by the
    // mask. Can't add a config that was previously removed.
    void narrowConfigs(const ConfigSet& mask) { configs_ &= mask; }

    // Current set of active creatives for a given impression.
    CreativeMatrix creatives(unsigned impId) const
    {
        CreativeMatrix mask(configs_);
        mask &= creatives_[impId];
        return mask;
    }

    // Restricts the number of active creatives to those specified by the mask
    // for the given impression. Can't add a creative that was previously
    // removed. Will also restrict the configs accordingly.
    void narrowCreativesForImp(unsigned impId, const CreativeMatrix& mask)
    {
        creatives_[impId] &= mask;
        updateConfigs();
    }

    // Restricts the number of active creatives to those specified by the mask
    // for all impressions. Can't add a creative that was previously
    // removed. Will also restrict the configs accordingly.
    void narrowAllCreatives(const CreativeMatrix& mask)
    {
        for (CreativeMatrix& matrix : creatives_) matrix &= mask;
        updateConfigs();
    }

    // Returns a map of configIndex to BiddableSpots object based on the
    // creative matrix. This is the format ingested by the router. Configs
    // that have no creative left are not part of the result.
    std::unordered_map<unsigned, BiddableSpots> biddableSpots();

    /*
     * This map is keyed by filtered reasons and contains a ConfigSet.
     * Of course, those configs belonging to those configIndex were filtered
     * due to the given reason.
     */
    typedef std::map<std::string, ConfigSet> FilterReasons;

    FilterReasons& getFilterReasons() { return filterReasons_; }

    void resetFilterReasons() { filterReasons_.clear(); }

private:
    void updateConfigs()
    {
        CreativeMatrix mask;
        for (const CreativeMatrix& matrix : creatives_) mask |= matrix;
        configs_ &= mask.aggregate();
    }

    ConfigSet configs_;
    std::vector<CreativeMatrix> creatives_;
    FilterReasons filterReasons_;
};


/******************************************************************************/
/* FILTER BASE                                                                */
/******************************************************************************/

/** Filtering interface for all filters.

    Note that this class should never be used directly. Prefer the FilterBaseT
    templates (generic_filters.h) instead since they provide useful default
    implementation for various technicalities. */
struct FilterBase
{
    virtual ~FilterBase() = default;

    virtual std::string name() const = 0;

    /** Determine in which order filters are executed. Priority is ascending (0
        is the first). */
    virtual unsigned priority() const { return 0; }

    /** Filters the given bid request. The filter should modify state to
        filter-out configs. */
    virtual void filter(FilterState& state) const = 0;

    /** Indicates that a new config is available and that it is associated with
        the given index. The configIndex should be used to manipulate the
        FilterState object during filtering. */
    virtual void
    addConfig(unsigned configIndex, const std::shared_ptr<AgentConfig>& config) = 0;

    /** Indicates that an existing config was removed and that it is associated
        with the given index. */
    virtual void
    removeConfig(unsigned configIndex, const std::shared_ptr<AgentConfig>& config) = 0;
};

} // namespace RTBKIT

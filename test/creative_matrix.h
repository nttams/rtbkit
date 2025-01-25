#pragma once

#include "configset.h"

struct CreativeMatrix
{
    explicit CreativeMatrix(bool defaultValue = false) : defaultValue(ConfigSet(defaultValue))
    {
    }

    explicit CreativeMatrix(ConfigSet defaultValue) : defaultValue(defaultValue)
    {
    }

    size_t size() const { return matrix.size(); }

    bool empty() const
    {
        for (const ConfigSet &set : matrix)
        {
            if (!set.empty())
                return false;
        }
        return true;
    }

    void expand(size_t newSize)
    {
        if (newSize <= matrix.size())
            return;
        matrix.resize(newSize, defaultValue);
    }

    const ConfigSet &operator[](size_t creative) const
    {
        return matrix[creative];
    }

    bool test(size_t creative, size_t config) const
    {
        if (creative >= matrix.size())
            return defaultValue.test(config);
        return matrix[creative].test(config);
    }

    void set(size_t creative, size_t config, bool value = true)
    {
        expand(creative + 1);
        matrix[creative].set(config, value);
    }

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
        for (size_t cr = 0; cr < size(); ++cr)
            matrix[cr].reset(config);
    }

#define RTBKIT_CREATIVE_MATRIX_OP(_op_)                              \
    CreativeMatrix &operator _op_(const CreativeMatrix & other)      \
    {                                                                \
        expand(other.matrix.size());                                 \
                                                                     \
        for (size_t i = 0; i < other.matrix.size(); ++i)             \
            matrix[i] _op_ other.matrix[i];                          \
                                                                     \
        for (size_t i = other.matrix.size(); i < matrix.size(); ++i) \
            matrix[i] _op_ other.defaultValue;                       \
                                                                     \
        return *this;                                                \
    }

    RTBKIT_CREATIVE_MATRIX_OP(&=)
    RTBKIT_CREATIVE_MATRIX_OP(|=)
    RTBKIT_CREATIVE_MATRIX_OP(^=)

#undef RTBKIT_CREATIVE_MATRIX_OP

    // The bit-wise not(~) operator. There's a good reason why this isn't a
    // operator overload but I can't remember it.
    CreativeMatrix &negate()
    {
        defaultValue = defaultValue.negate();
        for (ConfigSet &set : matrix)
            set.negate();
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

        for (const ConfigSet &set : matrix)
            configs |= set;

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
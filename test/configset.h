#pragma once

#include <sstream>

struct ConfigSet
{
    typedef uint64_t Word;
    static constexpr size_t Div = sizeof(Word) * 8;

    explicit ConfigSet(bool defaultValue = false) : defaultValue(defaultValue ? ~Word(0) : 0)
    {
    }

    size_t size() const
    {
        return bitfield.size() * Div;
    }

    // Expands the set to contain at least newSize configs and use the
    // defaultValue provided to the constructor to initialize the bit field.
    void expand(size_t newSize)
    {
        if (newSize)
            newSize = (newSize - 1) / Div + 1; // ceilDiv(newSize, Div)
        if (newSize <= bitfield.size())
            return;
        bitfield.resize(newSize, defaultValue);
    }

    void set(size_t index)
    {
        expand(index + 1);
        bitfield[index / Div] |= 1ULL << (index % Div);
    }

    void set(size_t index, bool value)
    {
        if (value)
            set(index);
        else
            reset(index);
    }

    void reset(size_t index)
    {
        expand(index + 1);
        bitfield[index / Div] &= ~(1ULL << (index % Div));
    }

    bool operator[](size_t index) const { return test(index); }

    bool test(size_t index) const
    {
        if (index >= size())
            return defaultValue;
        return bitfield[index / Div] & (1ULL << (index % Div));
    }

    size_t count() const
    {
        size_t total = 0;

        for (size_t i = 0; i < bitfield.size(); ++i)
        {
            if (!bitfield[i])
                continue;
            total += __builtin_popcount(bitfield[i]);
        }

        return total;
    }

    size_t empty() const
    {
        if (bitfield.empty())
            return !defaultValue;

        for (size_t i = 0; i < bitfield.size(); ++i)
        {
            if (bitfield[i])
                return false;
        }
        return true;
    }

#define RTBKIT_CONFIG_SET_OP(_op_)                                       \
    ConfigSet &operator _op_(const ConfigSet & other)                    \
    {                                                                    \
        expand(other.size());                                            \
                                                                         \
        for (size_t i = 0; i < other.bitfield.size(); ++i)               \
            bitfield[i] _op_ other.bitfield[i];                          \
                                                                         \
        for (size_t i = other.bitfield.size(); i < bitfield.size(); ++i) \
            bitfield[i] _op_ other.defaultValue;                         \
                                                                         \
        return *this;                                                    \
    }

    RTBKIT_CONFIG_SET_OP(&=)
    RTBKIT_CONFIG_SET_OP(|=)
    RTBKIT_CONFIG_SET_OP(^=)

#undef RTBKIT_CONFIG_SET_OP

#define RTBKIT_CONFIG_SET_OP_CONST(_op_)                  \
    ConfigSet operator _op_(const ConfigSet &other) const \
    {                                                     \
        ConfigSet tmp = *this;                            \
        tmp _op_## = other;                               \
        return tmp;                                       \
    }

    RTBKIT_CONFIG_SET_OP_CONST(&)
    RTBKIT_CONFIG_SET_OP_CONST(|)
    RTBKIT_CONFIG_SET_OP_CONST(^)

#undef RTBKIT_CONFIG_SET_OP_CONST

    ConfigSet &negate()
    {
        defaultValue = ~defaultValue;
        for (size_t i = 0; i < bitfield.size(); ++i)
            bitfield[i] = ~bitfield[i];
        return *this;
    }

    ConfigSet negate() const
    {
        return ConfigSet(*this).negate();
    }

    size_t next(size_t start = 0) const
    {
        size_t topIndex = start / Div;
        size_t subIndex = start % Div;
        Word mask = -1ULL & ~((1ULL << subIndex) - 1);

        for (size_t i = topIndex; i < bitfield.size(); ++i)
        {
            Word value = bitfield[i] & mask;
            mask = -1ULL;

            if (!value)
                continue;

            return (i * Div) + __builtin_ctz(value);
        }

        return size();
    }

    std::string print() const
    {
        std::stringstream ss;
        ss << "{ " << std::hex;
        for (Word w : bitfield)
            ss << w << " ";
        ss << "d:" << (defaultValue ? "1" : "0") << " ";
        ss << "}";
        return ss.str();
    }

private:
    std::vector<Word> bitfield;
    Word defaultValue;
};
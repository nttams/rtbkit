// Tiny replacement for jml/utils/exc_check.h.
#pragma once

#include <stdexcept>
#include <string>

#define ExcAssert(_expr_)                                                  \
    do {                                                                   \
        if (!(_expr_))                                                     \
            throw std::logic_error("assertion failed: " #_expr_);          \
    } while (0)

#define ExcCheck(_expr_, _msg_)                                            \
    do {                                                                   \
        if (!(_expr_)) throw std::runtime_error(_msg_);                    \
    } while (0)

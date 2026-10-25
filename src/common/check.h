// Small assertion helpers that throw instead of aborting.
#pragma once

#include <stdexcept>
#include <string>

#define RTB_ASSERT(_expr_)                                                 \
    do {                                                                   \
        if (!(_expr_))                                                     \
            throw std::logic_error("assertion failed: " #_expr_);          \
    } while (0)

#define RTB_CHECK(_expr_, _msg_)                                           \
    do {                                                                   \
        if (!(_expr_)) throw std::runtime_error(_msg_);                    \
    } while (0)

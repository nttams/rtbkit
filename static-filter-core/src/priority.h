// Port of rtbkit/core/router/filters/priority.h (static filters only).
#pragma once

namespace RTBKIT {

struct Priority
{
    static constexpr unsigned ExchangeName  = 0x0200;

    static constexpr unsigned Location      = 0x1000;
    static constexpr unsigned Language      = 0x1100;
    static constexpr unsigned Host          = 0x1200;
    static constexpr unsigned Url           = 0x1300;

    static constexpr unsigned Segments      = 0x3000;
    static constexpr unsigned HourOfWeek    = 0x3100;
    static constexpr unsigned FoldPosition  = 0x3200;
    static constexpr unsigned RequiredIds   = 0x3300;
    static constexpr unsigned UserPartition = 0x3400;

    static constexpr unsigned ExchangePre   = 0xF000;

    static constexpr unsigned LatLong       = 0xF200;

    static constexpr unsigned ExchangePost  = 0xFF00;
};

} // namespace RTBKIT

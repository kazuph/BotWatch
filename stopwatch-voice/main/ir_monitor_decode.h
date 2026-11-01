#pragma once
#include <cstddef>
#include <cstdint>

inline bool decode_monitor_nec(const uint32_t *intervals, std::size_t count, uint32_t &code)
{
    if (count != 67 || intervals[0] < 8000 || intervals[0] > 10000 ||
        intervals[1] < 4000 || intervals[1] > 5000) return false;
    uint32_t result = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const uint32_t mark = intervals[2 + i * 2], space = intervals[3 + i * 2];
        if (mark < 350 || mark > 800 || !((space >= 350 && space <= 800) || (space >= 1300 && space <= 2000))) return false;
        result = (result << 1) | (space > 1125);
    }
    if (intervals[66] < 350 || intervals[66] > 800) return false;
    code = result;
    return true;
}

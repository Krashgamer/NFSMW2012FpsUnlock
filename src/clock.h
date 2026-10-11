#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Wall clock in 100 ns units, the unit the engine's frame clock uses.
inline int64_t QpcFrequency()
{
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    return freq;
}

inline int64_t Now100ns()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t freq = QpcFrequency();
    const int64_t qpc = now.QuadPart;
    // Overflow-safe: seconds * 1e7 + (remainder * 1e7) / freq.
    return (qpc / freq) * 10000000 + (qpc % freq) * 10000000 / freq;
}

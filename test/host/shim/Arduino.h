#pragma once
// Minimal Arduino environment so tapo_client.cpp can be compiled and tested on a PC.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "WString.h"

inline uint32_t millis() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

using std::max;
using std::min;

struct HostSerial {
    void print(const String& s) { fputs(s.c_str(), stdout); }
    void print(const char* s) { fputs(s, stdout); }
    void println(const String& s) { puts(s.c_str()); }
    void println(const char* s = "") { puts(s); }
    int printf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int n = vprintf(fmt, ap);
        va_end(ap);
        return n;
    }
};
extern HostSerial Serial;

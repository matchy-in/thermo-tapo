#pragma once
#include <sys/random.h>
#include <cstddef>

inline void esp_fill_random(void* buf, size_t len) { (void)!getrandom(buf, len, 0); }

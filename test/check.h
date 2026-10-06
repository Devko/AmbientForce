// From SubForce test/check.h (8846421), sft -> aft.
#pragma once
// The tiny check framework every test file shares (counters live in plugin_test.cpp).
#include <cstdio>

namespace aft {
extern int g_fail, g_pass;
}

#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (c) ++aft::g_pass;                                                             \
        else { ++aft::g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }  \
    } while (0)

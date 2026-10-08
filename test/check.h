// From SubForce test/check.h (8846421), sft -> aft; the allocation counter added.
#pragma once
// The tiny check framework every test file shares (counters live in plugin_test.cpp).
#include <cstddef>
#include <cstdio>

namespace aft {
extern int g_fail, g_pass;
}

// Counting allocations (the suites' "nothing allocating" checks, under ASan: make test). The
// sanitizer's allocator calls a hook on every allocation and free; compiler-rt keeps only a few such
// hooks a process, so one is installed, once, for every suite (hookAllocations()), and it counts the
// allocations made on the thread that counts while it counts. AFT_COUNTS_ALLOCS is 0 where there is
// no sanitizer to hook (the ARM build).
#if defined(__SANITIZE_ADDRESS__)
#define AFT_COUNTS_ALLOCS 1
// The sanitizer runtime's (sanitizer/allocator_interface.h, which GCC doesn't install).
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                                         void (*free_hook)(const volatile void*));
namespace aft {
inline thread_local bool t_countingAllocs = false;
inline int g_allocs = 0;
inline void onMallocHook(const volatile void*, size_t) {
    if (t_countingAllocs) ++g_allocs;
}
inline void onFreeHook(const volatile void*) {}
// The hooks, installed the first time any suite asks; false if the runtime refused them.
inline bool hookAllocations() {
    static const bool hooked = __sanitizer_install_malloc_and_free_hooks(onMallocHook, onFreeHook) != 0;
    return hooked;
}
// From here this thread's allocations count, from 0; then stop and read them.
inline void countAllocations() {
    g_allocs = 0;
    t_countingAllocs = true;
}
inline int allocationsCounted() {
    t_countingAllocs = false;
    return g_allocs;
}
} // namespace aft
#else
#define AFT_COUNTS_ALLOCS 0
#endif

#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (c) ++aft::g_pass;                                                             \
        else { ++aft::g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }  \
    } while (0)

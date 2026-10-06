// From EffectForce test/module_main.cpp (4160e87), eft -> aft; the suite in namespace aft.
// One dsp suite on its own: make test-module M=<suite> builds this with -DMODULE_TESTS=<suite>Tests,
// test/<suite>_test.cpp and every dsp/*.cpp. The full suite (make test) calls every suite from
// plugin_test.cpp's main.
#include "check.h"

#include <cstdio>

int aft::g_fail = 0, aft::g_pass = 0;

namespace aft {
void MODULE_TESTS();
}

int main() {
    aft::MODULE_TESTS();
    std::printf("%d passed, %d failed\n", aft::g_pass, aft::g_fail);
    return aft::g_fail ? 1 : 0;
}

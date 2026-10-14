#ifndef USBDIRECT_TEST_CHECK_H
#define USBDIRECT_TEST_CHECK_H
#include <cstdio>
#include <cstdlib>
/* Assertion that survives NDEBUG and counts executions. */
static long g_checks = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                            \
        }                                                                            \
    } while (0)
#define PASS(name) std::printf("PASS %s (%ld assertions)\n", name, g_checks)
#endif

#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int t_total = 0;
static int t_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        t_total++;                                                               \
        if (!(cond)) {                                                           \
            t_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define CHECK_STR(actual, expected)                                              \
    do {                                                                         \
        t_total++;                                                               \
        const char *a_ = (actual), *e_ = (expected);                             \
        if (strcmp(a_, e_) != 0) {                                               \
            t_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n  expected: \"%s\"\n  actual:   \"%s\"\n", \
                    __FILE__, __LINE__, #actual, e_, a_);                        \
        }                                                                        \
    } while (0)

static int t_report(const char *name) {
    printf("%s: %d checks, %d failed\n", name, t_total, t_failed);
    return t_failed ? 1 : 0;
}

#endif

/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_TEST_H
#define PEABERRY_TEST_H

#include <math.h>
#include <stdio.h>

typedef struct pb_test_stats {
    int passed;
    int failed;
    int skipped;
} pb_test_stats;

extern pb_test_stats g_pb_test_stats;

void pb_test_reset_stats(void);
void pb_test_report(const char *suite);

#define PB_TEST(name) \
    static void name(void); \
    static void run_##name(void) \
    { \
        printf("  %s ... ", #name); \
        name(); \
    } \
    static void name(void)

#define PB_TEST_PASS() \
    do { \
        g_pb_test_stats.passed++; \
        printf("ok\n"); \
        return; \
    } while (0)

#define PB_TEST_FAIL(msg) \
    do { \
        g_pb_test_stats.failed++; \
        printf("FAIL (%s)\n", msg); \
        return; \
    } while (0)

#define PB_TEST_SKIP(msg) \
    do { \
        g_pb_test_stats.skipped++; \
        printf("skip (%s)\n", msg); \
        return; \
    } while (0)

#define PB_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            PB_TEST_FAIL(#cond); \
        } \
    } while (0)

#define PB_ASSERT_FLOAT_EQ(a, b, eps) \
    do { \
        const float _a = (a); \
        const float _b = (b); \
        const float _eps = (eps); \
        if (fabsf(_a - _b) > _eps) { \
            PB_TEST_FAIL(#a " ~= " #b); \
        } \
    } while (0)

#define PB_RUN_TEST(name) run_##name()

#endif

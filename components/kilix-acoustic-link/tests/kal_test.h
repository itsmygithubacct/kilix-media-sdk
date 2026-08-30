/* Minimal test harness. Every count is reported with its denominator. */
#ifndef KAL_TEST_H
#define KAL_TEST_H

#include <stdio.h>
#include <stdlib.h>

static unsigned long kal_test_passed;
static unsigned long kal_test_total;
static unsigned long kal_group_passed;
static unsigned long kal_group_total;

#define KAL_CHECK(condition) \
    do { \
        kal_test_total++; \
        if (condition) { \
            kal_test_passed++; \
        } else { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        } \
    } while (0)

#define KAL_GROUP(name, passed, total) \
    do { \
        kal_group_passed += (unsigned long)(passed); \
        kal_group_total += (unsigned long)(total); \
        printf("  %-44s %lu/%lu\n", (name), (unsigned long)(passed), \
               (unsigned long)(total)); \
    } while (0)

static int kal_test_report(const char *suite) {
    printf("%s assertions %lu/%lu items %lu/%lu\n", suite,
           kal_test_passed, kal_test_total, kal_group_passed, kal_group_total);
    if (kal_test_passed != kal_test_total || kal_group_passed != kal_group_total) {
        printf("%s RESULT FAIL %lu/%lu\n", suite, kal_test_passed, kal_test_total);
        return 1;
    }
    printf("%s RESULT PASS %lu/%lu\n", suite, kal_test_passed, kal_test_total);
    return 0;
}

#endif /* KAL_TEST_H */

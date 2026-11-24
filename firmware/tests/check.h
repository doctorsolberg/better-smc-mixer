/* Minimal test helpers for host unit tests. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

extern int check_failed;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            check_failed++;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);    \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        long long a_ = (long long)(a), b_ = (long long)(b);                          \
        if (a_ != b_) {                                                              \
            check_failed++;                                                          \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__,      \
                    __LINE__, #a, #b, a_, b_);                                       \
        }                                                                            \
    } while (0)

#define RUN(test)                                                                    \
    do {                                                                             \
        int before_ = check_failed;                                                  \
        test();                                                                      \
        printf("%s %s\n", check_failed == before_ ? "ok  " : "FAIL", #test);         \
    } while (0)

#endif /* CHECK_H */

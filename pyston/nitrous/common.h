#include <stdlib.h>

#ifndef _NITROUS_COMMON_H
#define _NITROUS_COMMON_H

#define _STRINGIFY(N) #N
#define STRINGIFY(N) _STRINGIFY(N)
#define RELEASE_ASSERT(condition, fmt, ...)                                                                            \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ::fprintf(stderr, __FILE__ ":" STRINGIFY(__LINE__) ": %s: Assertion `" #condition "' failed: " fmt "\n",   \
                      __PRETTY_FUNCTION__, ##__VA_ARGS__);                                                             \
            ::abort();                                                                                                 \
        }                                                                                                              \
    } while (false)

#define NITROUS_VERBOSITY_NONE 0
#define NITROUS_VERBOSITY_STATS 1
#define NITROUS_VERBOSITY_IR 2
#define NITROUS_VERBOSITY_INTERPRETING 3
extern int nitrous_verbosity;
extern bool nitrous_pic;

#endif

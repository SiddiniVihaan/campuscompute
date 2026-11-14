#include "common.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_verbose = 1;

/* Strict integer parse: the whole string must be a base-10 int. */
int parse_int(const char *s, int *out)
{
    if (s == NULL || *s == '\0')
        return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

void print_header(const char *title)
{
    if (!g_verbose)
        return;
    size_t len = strlen(title);
    printf("\n");
    for (size_t i = 0; i < len + 8; i++) putchar('=');
    printf("\n=== %s ===\n", title);
    for (size_t i = 0; i < len + 8; i++) putchar('=');
    printf("\n");
}

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

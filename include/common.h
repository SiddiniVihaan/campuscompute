#ifndef CAMPUS_COMMON_H
#define CAMPUS_COMMON_H

#include <stdio.h>

/* When 0, modules print only one-line results (used by the integrated demo). */
extern int g_verbose;

#define INFO(...) do { if (g_verbose) printf(__VA_ARGS__); } while (0)

#define EXIT_OK      0
#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

int  parse_int(const char *s, int *out);
void print_header(const char *title);
double now_seconds(void);
void sleep_ms(int ms);

#endif

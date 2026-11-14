/*
 * Banker's safety algorithm.
 *
 * File format (integers, whitespace separated, '#' comments allowed):
 *   n m                      processes, resource types
 *   Available[m]
 *   Max[n][m]                one row per process
 *   Allocation[n][m]         one row per process
 */
#include "banker.h"
#include "common.h"
#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MAX_TOKENS (2 + BK_MAX_R + 2 * BK_MAX_P * BK_MAX_R + 1)

static int read_ints(const char *path, int *vals, int max, int *count)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open banker file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[512];
    int lineno = 0, n = 0, bad = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
            int v;
            if (parse_int(t, &v) != 0) {
                fprintf(stderr, "  %s:%d: '%s' is not an integer\n", path, lineno, t);
                bad = 1;
                continue;
            }
            if (n >= max) {
                fprintf(stderr, "  %s:%d: too many values\n", path, lineno);
                bad = 1;
                break;
            }
            vals[n++] = v;
        }
    }
    fclose(fp);
    *count = n;
    return bad ? -1 : 0;
}

int banker_load(const char *path, BankerState *s)
{
    int v[MAX_TOKENS], cnt = 0;
    memset(s, 0, sizeof *s);
    if (read_ints(path, v, MAX_TOKENS, &cnt) != 0) {
        fprintf(stderr, "error: banker file '%s' rejected\n", path);
        return -1;
    }
    if (cnt < 2) {
        fprintf(stderr, "error: %s: missing 'n m' header\n", path);
        return -1;
    }
    s->n = v[0];
    s->m = v[1];
    if (s->n < 1 || s->n > BK_MAX_P || s->m < 1 || s->m > BK_MAX_R) {
        fprintf(stderr, "error: %s: need 1..%d processes and 1..%d resource types\n",
                path, BK_MAX_P, BK_MAX_R);
        return -1;
    }
    int expected = 2 + s->m + 2 * s->n * s->m;
    if (cnt != expected) {
        fprintf(stderr, "error: %s: expected %d values for n=%d m=%d, found %d\n",
                path, expected, s->n, s->m, cnt);
        return -1;
    }
    int k = 2;
    for (int r = 0; r < s->m; r++) s->available[r] = v[k++];
    for (int p = 0; p < s->n; p++)
        for (int r = 0; r < s->m; r++) s->max[p][r] = v[k++];
    for (int p = 0; p < s->n; p++)
        for (int r = 0; r < s->m; r++) s->alloc[p][r] = v[k++];

    int errors = 0;
    for (int r = 0; r < s->m; r++)
        if (s->available[r] < 0) {
            fprintf(stderr, "error: Available[%c] is negative\n", 'A' + r);
            errors++;
        }
    for (int p = 0; p < s->n; p++)
        for (int r = 0; r < s->m; r++) {
            if (s->max[p][r] < 0 || s->alloc[p][r] < 0) {
                fprintf(stderr, "error: P%d resource %c: negative value\n", p, 'A' + r);
                errors++;
            } else if (s->alloc[p][r] > s->max[p][r]) {
                fprintf(stderr, "error: P%d resource %c: Allocation %d exceeds Maximum %d\n",
                        p, 'A' + r, s->alloc[p][r], s->max[p][r]);
                errors++;
            }
            s->need[p][r] = s->max[p][r] - s->alloc[p][r];
        }
    return errors ? -1 : 0;
}

static void print_vec(const int *v, int m)
{
    printf("[");
    for (int r = 0; r < m; r++) printf("%s%d", r ? " " : "", v[r]);
    printf("]");
}

static int vec_le(const int *a, const int *b, int m)
{
    for (int r = 0; r < m; r++)
        if (a[r] > b[r]) return 0;
    return 1;
}

/*
 * Repeated passes over P0..Pn-1: any unfinished process whose Need <= Work
 * can finish and release its allocation into Work. If a whole pass makes no
 * progress while processes remain unfinished, the state is unsafe.
 */
int banker_is_safe(const BankerState *s, int *seq, int *seqlen)
{
    int work[BK_MAX_R], finish[BK_MAX_P] = {0};
    memcpy(work, s->available, sizeof work);
    int count = 0, step = 0, progress = 1;

    INFO("\nSafety algorithm trace (Work starts as Available = ");
    if (g_verbose) print_vec(work, s->m);
    INFO("):\n");

    while (count < s->n && progress) {
        progress = 0;
        for (int p = 0; p < s->n; p++) {
            if (finish[p] || !vec_le(s->need[p], work, s->m))
                continue;
            if (g_verbose) {
                printf("  Step %d: P%d Need ", ++step, p);
                print_vec(s->need[p], s->m);
                printf(" <= Work ");
                print_vec(work, s->m);
                printf(" -> P%d can finish, releases ", p);
                print_vec(s->alloc[p], s->m);
            }
            for (int r = 0; r < s->m; r++) work[r] += s->alloc[p][r];
            if (g_verbose) {
                printf(", Work = ");
                print_vec(work, s->m);
                printf("\n");
            }
            finish[p] = 1;
            seq[count++] = p;
            progress = 1;
        }
    }
    *seqlen = count;

    if (count < s->n && g_verbose) {
        printf("  No unfinished process can proceed with Work = ");
        print_vec(work, s->m);
        printf(":\n");
        for (int p = 0; p < s->n; p++) {
            if (finish[p]) continue;
            printf("    P%d Need ", p);
            print_vec(s->need[p], s->m);
            printf(" > Work (short of");
            int first = 1;
            for (int r = 0; r < s->m; r++)
                if (s->need[p][r] > work[r]) {
                    printf("%s %c by %d", first ? ":" : ",", 'A' + r, s->need[p][r] - work[r]);
                    first = 0;
                }
            printf(")\n");
        }
    }
    return count == s->n;
}

int banker_run(const char *path, int *safe_out, char *seq_text, int seq_cap)
{
    BankerState s;
    if (banker_load(path, &s) != 0)
        return EXIT_RUNTIME;

    char title[160];
    snprintf(title, sizeof title, "Banker's safety algorithm: %s", path);
    print_header(title);

    if (g_verbose) {
        int total[BK_MAX_R] = {0};
        for (int r = 0; r < s.m; r++) {
            total[r] = s.available[r];
            for (int p = 0; p < s.n; p++) total[r] += s.alloc[p][r];
        }
        printf("Processes: %d   Resource types: %d (", s.n, s.m);
        for (int r = 0; r < s.m; r++) printf("%s%c", r ? " " : "", 'A' + r);
        printf(")\nTotal instances: ");
        print_vec(total, s.m);
        printf("   Available: ");
        print_vec(s.available, s.m);
        printf("\nNeed = Maximum - Allocation\n\n");

        int w = s.m * 3 + 4;
        printf("  %-5s %-*s %-*s %-*s\n", "Proc", w, "Allocation", w, "Maximum", w, "Need");
        for (int p = 0; p < s.n; p++) {
            printf("  P%-4d", p);
            const int *rows[3] = { s.alloc[p], s.max[p], s.need[p] };
            for (int c = 0; c < 3; c++) {
                printf(" ");
                for (int r = 0; r < s.m; r++) printf("%-3d", rows[c][r]);
                printf("%*s", 4, "");
            }
            printf("\n");
        }
    }

    int seq[BK_MAX_P], len = 0;
    int safe = banker_is_safe(&s, seq, &len);

    char buf[256] = "";
    int off = 0;
    for (int i = 0; i < len && off < (int)sizeof buf - 8; i++)
        off += snprintf(buf + off, sizeof buf - off, "%sP%d", i ? ", " : "", seq[i]);

    if (safe) {
        printf("\nResult: SAFE state. Safe sequence: <%s>\n", buf);
    } else {
        printf("\nResult: UNSAFE state. Only %d of %d processes can finish%s%s%s.\n",
               len, s.n, len ? " (" : "", buf, len ? ")" : "");
        printf("        No safe sequence exists - granting this state could lead to deadlock.\n");
    }
    logger_log("BANKER", "%s: %s%s%s", path, safe ? "SAFE, sequence " : "UNSAFE",
               safe ? buf : "", "");

    if (safe_out) *safe_out = safe;
    if (seq_text && seq_cap > 0)
        snprintf(seq_text, seq_cap, "%s", safe ? buf : "none");
    return EXIT_OK;
}

#include "jobs.h"
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *job_status_str(JobStatus s)
{
    switch (s) {
    case JOB_READY:   return "READY";
    case JOB_RUNNING: return "RUNNING";
    case JOB_DONE:    return "DONE";
    }
    return "?";
}

void jobs_reset(JobSet *set)
{
    for (int i = 0; i < set->count; i++) {
        Job *j = &set->jobs[i];
        j->remaining = j->burst;
        j->start = -1;
        j->completion = j->turnaround = j->waiting = j->response = 0;
        j->status = JOB_READY;
    }
}

static int check_field(const char *path, int lineno, const char *name,
                       const char *tok, int min, int max, int *out)
{
    if (parse_int(tok, out) != 0) {
        fprintf(stderr, "  %s:%d: %s '%s' is not an integer\n", path, lineno, name, tok);
        return -1;
    }
    if (*out < min || *out > max) {
        fprintf(stderr, "  %s:%d: %s %d out of range [%d..%d]\n",
                path, lineno, name, *out, min, max);
        return -1;
    }
    return 0;
}

/*
 * Job file format, one job per line:   id arrival burst priority
 * '#' starts a comment; blank lines are ignored. Any invalid line rejects
 * the whole file so that every run uses a fully validated, repeatable input.
 */
int jobs_load(const char *path, JobSet *set)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open job file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    memset(set, 0, sizeof *set);
    char line[512];
    int lineno = 0, errors = 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, "  %s:%d: line too long\n", path, lineno);
            errors++;
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF)
                ;
            continue;
        }
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *tok[5];
        int n = 0;
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\r\n", &save); t != NULL && n < 5;
             t = strtok_r(NULL, " \t\r\n", &save))
            tok[n++] = t;
        if (n == 0)
            continue;
        if (n != 4) {
            fprintf(stderr, "  %s:%d: expected 4 fields (id arrival burst priority), got %s%d\n",
                    path, lineno, n == 5 ? "more than " : "", n == 5 ? 4 : n);
            errors++;
            continue;
        }

        Job j;
        memset(&j, 0, sizeof j);
        if (check_field(path, lineno, "job id",   tok[0], 1, 9999, &j.id)           ||
            check_field(path, lineno, "arrival",  tok[1], 0, MAX_ARRIVAL, &j.arrival) ||
            check_field(path, lineno, "burst",    tok[2], 1, MAX_BURST, &j.burst)     ||
            check_field(path, lineno, "priority", tok[3], 0, MAX_PRIORITY, &j.priority)) {
            errors++;
            continue;
        }

        int dup = 0;
        for (int i = 0; i < set->count; i++)
            if (set->jobs[i].id == j.id) dup = 1;
        if (dup) {
            fprintf(stderr, "  %s:%d: duplicate job id %d\n", path, lineno, j.id);
            errors++;
            continue;
        }
        if (set->count >= MAX_JOBS) {
            fprintf(stderr, "  %s:%d: too many jobs (max %d)\n", path, lineno, MAX_JOBS);
            errors++;
            break;
        }
        set->jobs[set->count++] = j;
    }
    fclose(fp);

    if (errors > 0) {
        fprintf(stderr, "error: job file '%s' rejected: %d invalid line(s)\n", path, errors);
        return -1;
    }
    if (set->count == 0) {
        fprintf(stderr, "error: job file '%s' contains no jobs\n", path);
        return -1;
    }
    jobs_reset(set);
    return 0;
}

void jobs_print(const JobSet *set)
{
    printf("  %-5s %-8s %-6s %-9s %-8s\n", "Job", "Arrival", "Burst", "Priority", "Status");
    printf("  %-5s %-8s %-6s %-9s %-8s\n", "-----", "-------", "-----", "--------", "------");
    for (int i = 0; i < set->count; i++) {
        const Job *j = &set->jobs[i];
        printf("  J%-4d %-8d %-6d %-9d %-8s\n", j->id, j->arrival, j->burst, j->priority,
               job_status_str(j->status));
    }
    printf("  %d job(s) loaded and validated.\n", set->count);
}

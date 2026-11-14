#ifndef CAMPUS_WORKERS_H
#define CAMPUS_WORKERS_H

#define MAX_WORKERS      16
#define SIM_FAIL_EXIT    42   /* exit code reserved for the simulated failure */

typedef struct {
    int total;
    int completed;
    int failed;
    int simulated;
} WorkerSummary;

int workers_run(int count, int with_failures, WorkerSummary *sum);
int rlimit_run(const char *which, int *enforced);

#endif

#ifndef CAMPUS_SCHED_H
#define CAMPUS_SCHED_H

#include "jobs.h"

#define MAX_SLICES 4096
#define IDLE_SLICE (-1)

typedef enum { POLICY_FCFS, POLICY_RR, POLICY_PRIORITY } Policy;

typedef struct {
    int job_id;         /* IDLE_SLICE when the CPU has nothing to run */
    int start;
    int end;
} Slice;

typedef struct {
    Policy policy;
    int quantum;
    JobSet set;
    Slice slices[MAX_SLICES];
    int nslices;
    int truncated;
    int makespan;
    int busy_time;
    int dispatches;
    double avg_tat;
    double avg_wt;
    double avg_rt;
} SchedResult;

const char *policy_name(Policy p);
int  sched_run(Policy p, const JobSet *in, int quantum, SchedResult *out);
void sched_print(const SchedResult *r);
int  sched_compare(const JobSet *in, int quantum);

#endif

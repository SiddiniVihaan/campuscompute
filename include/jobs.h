#ifndef CAMPUS_JOBS_H
#define CAMPUS_JOBS_H

#define MAX_JOBS      64
#define MAX_ARRIVAL   10000
#define MAX_BURST     1000
#define MAX_PRIORITY  99

typedef enum { JOB_READY, JOB_RUNNING, JOB_DONE } JobStatus;

typedef struct {
    int id;
    int arrival;
    int burst;
    int priority;       /* lower number = higher priority */
    int remaining;
    int start;          /* first time on CPU, -1 if never ran */
    int completion;
    int turnaround;
    int waiting;
    int response;
    JobStatus status;
} Job;

typedef struct {
    Job jobs[MAX_JOBS];
    int count;
} JobSet;

int  jobs_load(const char *path, JobSet *set);
void jobs_reset(JobSet *set);
void jobs_print(const JobSet *set);
const char *job_status_str(JobStatus s);

#endif

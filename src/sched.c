#include "sched.h"
#include "common.h"
#include "logger.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *policy_name(Policy p)
{
    switch (p) {
    case POLICY_FCFS:     return "FCFS";
    case POLICY_RR:       return "Round Robin";
    case POLICY_PRIORITY: return "Priority (non-preemptive)";
    }
    return "?";
}

static const char *policy_rule(Policy p)
{
    switch (p) {
    case POLICY_FCFS:
        return "Jobs run in arrival order. Tie-break: equal arrival -> lower job ID first.";
    case POLICY_RR:
        return "FIFO ready queue. Jobs arriving during a slice are queued BEFORE the\n"
               "  preempted job. Tie-break: equal arrival -> lower job ID first.";
    case POLICY_PRIORITY:
        return "Lower number = higher priority; a running job is never preempted.\n"
               "  Tie-break: equal priority -> earlier arrival -> lower job ID.";
    }
    return "";
}

/* Appends a slice, merging with the previous one if it is the same job continuing. */
static void add_slice(SchedResult *r, int job_id, int start, int end)
{
    if (end <= start)
        return;
    if (r->nslices > 0) {
        Slice *last = &r->slices[r->nslices - 1];
        if (last->job_id == job_id && last->end == start) {
            last->end = end;
            return;
        }
    }
    if (job_id != IDLE_SLICE)
        r->dispatches++;
    if (r->nslices >= MAX_SLICES) {
        r->truncated = 1;
        return;
    }
    r->slices[r->nslices].job_id = job_id;
    r->slices[r->nslices].start = start;
    r->slices[r->nslices].end = end;
    r->nslices++;
}

/* Index order sorted by (arrival, id) - insertion sort, n <= MAX_JOBS. */
static void order_by_arrival(const JobSet *s, int *idx)
{
    for (int i = 0; i < s->count; i++) {
        int k = i;
        idx[i] = i;
        while (k > 0) {
            const Job *a = &s->jobs[idx[k - 1]], *b = &s->jobs[idx[k]];
            if (a->arrival < b->arrival || (a->arrival == b->arrival && a->id < b->id))
                break;
            int tmp = idx[k - 1]; idx[k - 1] = idx[k]; idx[k] = tmp;
            k--;
        }
    }
}

static void dispatch(Job *j, int t)
{
    if (j->start < 0) {
        j->start = t;
        j->response = t - j->arrival;
    }
    j->status = JOB_RUNNING;
}

static void complete(Job *j, int t)
{
    j->completion = t;
    j->turnaround = t - j->arrival;
    j->waiting = j->turnaround - j->burst;
    j->remaining = 0;
    j->status = JOB_DONE;
}

static void run_fcfs(SchedResult *r)
{
    int idx[MAX_JOBS];
    order_by_arrival(&r->set, idx);
    int t = 0;
    for (int k = 0; k < r->set.count; k++) {
        Job *j = &r->set.jobs[idx[k]];
        if (t < j->arrival) {
            add_slice(r, IDLE_SLICE, t, j->arrival);
            t = j->arrival;
        }
        dispatch(j, t);
        add_slice(r, j->id, t, t + j->burst);
        t += j->burst;
        complete(j, t);
    }
}

static int higher_priority(const Job *a, const Job *b)
{
    if (a->priority != b->priority) return a->priority < b->priority;
    if (a->arrival != b->arrival)   return a->arrival < b->arrival;
    return a->id < b->id;
}

static void run_priority(SchedResult *r)
{
    Job *J = r->set.jobs;
    int n = r->set.count, done = 0, t = 0;
    while (done < n) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (J[i].status == JOB_DONE || J[i].arrival > t)
                continue;
            if (best < 0 || higher_priority(&J[i], &J[best]))
                best = i;
        }
        if (best < 0) {
            /* Nothing has arrived yet: the CPU idles until the next arrival. */
            int next = INT_MAX;
            for (int i = 0; i < n; i++)
                if (J[i].status != JOB_DONE && J[i].arrival < next)
                    next = J[i].arrival;
            add_slice(r, IDLE_SLICE, t, next);
            t = next;
            continue;
        }
        dispatch(&J[best], t);
        add_slice(r, J[best].id, t, t + J[best].burst);
        t += J[best].burst;
        complete(&J[best], t);
        done++;
    }
}

static void run_rr(SchedResult *r)
{
    Job *J = r->set.jobs;
    int n = r->set.count, q = r->quantum;
    int idx[MAX_JOBS];
    order_by_arrival(&r->set, idx);

    /* Circular ready queue; each job is in the queue at most once, so n slots suffice. */
    int queue[MAX_JOBS], head = 0, tail = 0, qlen = 0;
    int next = 0, done = 0, t = 0;

#define ENQUEUE(i) do { queue[tail] = (i); tail = (tail + 1) % MAX_JOBS; qlen++; } while (0)
#define ADMIT_UPTO(time) \
    while (next < n && J[idx[next]].arrival <= (time)) ENQUEUE(idx[next++])

    ADMIT_UPTO(t);
    while (done < n) {
        if (qlen == 0) {
            int a = J[idx[next]].arrival;
            add_slice(r, IDLE_SLICE, t, a);
            t = a;
            ADMIT_UPTO(t);
            continue;
        }
        int i = queue[head];
        head = (head + 1) % MAX_JOBS;
        qlen--;

        Job *j = &J[i];
        int run = j->remaining < q ? j->remaining : q;
        dispatch(j, t);
        add_slice(r, j->id, t, t + run);
        t += run;
        j->remaining -= run;

        /* Arrivals up to and including t join the queue before the preempted job. */
        ADMIT_UPTO(t);
        if (j->remaining > 0) {
            j->status = JOB_READY;
            ENQUEUE(i);
        } else {
            complete(j, t);
            done++;
        }
    }
#undef ADMIT_UPTO
#undef ENQUEUE
}

int sched_run(Policy p, const JobSet *in, int quantum, SchedResult *out)
{
    memset(out, 0, sizeof *out);
    out->policy = p;
    out->quantum = quantum;
    out->set = *in;
    jobs_reset(&out->set);

    switch (p) {
    case POLICY_FCFS:     run_fcfs(out); break;
    case POLICY_RR:       run_rr(out); break;
    case POLICY_PRIORITY: run_priority(out); break;
    }

    int n = out->set.count;
    double tat = 0, wt = 0, rt = 0;
    for (int i = 0; i < n; i++) {
        const Job *j = &out->set.jobs[i];
        tat += j->turnaround;
        wt += j->waiting;
        rt += j->response;
        out->busy_time += j->burst;
        if (j->completion > out->makespan)
            out->makespan = j->completion;
    }
    out->avg_tat = tat / n;
    out->avg_wt = wt / n;
    out->avg_rt = rt / n;

    logger_log("SCHED", "%s%s on %d jobs: avgTAT=%.2f avgWT=%.2f avgRT=%.2f makespan=%d",
               policy_name(p), p == POLICY_RR ? " (quantum set)" : "", n,
               out->avg_tat, out->avg_wt, out->avg_rt, out->makespan);
    return 0;
}

static void print_gantt(const SchedResult *r)
{
    const int per_row = 10;
    char label[16];
    for (int s = 0; s < r->nslices; s += per_row) {
        int e = s + per_row < r->nslices ? s + per_row : r->nslices;
        printf("  ");
        for (int k = s; k < e; k++) {
            if (r->slices[k].job_id == IDLE_SLICE)
                snprintf(label, sizeof label, "IDLE");
            else
                snprintf(label, sizeof label, "J%d", r->slices[k].job_id);
            printf("| %-5s", label);
        }
        printf("|\n  ");
        for (int k = s; k < e; k++)
            printf("%-7d", r->slices[k].start);
        printf("%d\n", r->slices[e - 1].end);
    }
    if (r->truncated)
        printf("  (timeline truncated after %d slices; metrics are still complete)\n", MAX_SLICES);
}

void sched_print(const SchedResult *r)
{
    char title[64];
    if (r->policy == POLICY_RR)
        snprintf(title, sizeof title, "%s (time quantum = %d)", policy_name(r->policy), r->quantum);
    else
        snprintf(title, sizeof title, "%s", policy_name(r->policy));
    print_header(title);

    printf("Rule: %s\n", policy_rule(r->policy));
    if (r->policy == POLICY_RR)
        printf("Time quantum: %d time unit(s)\n", r->quantum);

    printf("\nExecution order:\n  ");
    int first = 1;
    for (int k = 0; k < r->nslices; k++) {
        if (r->slices[k].job_id == IDLE_SLICE) continue;
        printf("%sJ%d", first ? "" : " -> ", r->slices[k].job_id);
        first = 0;
    }
    printf("\n\nTimeline (Gantt chart):\n");
    print_gantt(r);

    printf("\n  %-5s %-7s %-5s %-5s %-6s %-6s %-6s %-6s %-6s %-7s\n",
           "Job", "Arrive", "Burst", "Prio", "Start", "CT", "TAT", "WT", "RT", "Status");
    printf("  %-5s %-7s %-5s %-5s %-6s %-6s %-6s %-6s %-6s %-7s\n",
           "-----", "------", "-----", "----", "-----", "----", "----", "----", "----", "------");
    for (int i = 0; i < r->set.count; i++) {
        const Job *j = &r->set.jobs[i];
        printf("  J%-4d %-7d %-5d %-5d %-6d %-6d %-6d %-6d %-6d %-7s\n",
               j->id, j->arrival, j->burst, j->priority, j->start, j->completion,
               j->turnaround, j->waiting, j->response, job_status_str(j->status));
    }
    printf("\n  CT = completion, TAT = CT - arrival, WT = TAT - burst, RT = first start - arrival\n");
    printf("  Average TAT = %.2f   Average WT = %.2f   Average RT = %.2f\n",
           r->avg_tat, r->avg_wt, r->avg_rt);
    printf("  Makespan = %d   CPU busy = %d   CPU utilisation = %.1f%%   Dispatches = %d\n",
           r->makespan, r->busy_time, 100.0 * r->busy_time / r->makespan, r->dispatches);
}

int sched_compare(const JobSet *in, int quantum)
{
    static SchedResult res[3];   /* large structs: keep off the stack */
    Policy pol[3] = { POLICY_FCFS, POLICY_RR, POLICY_PRIORITY };
    for (int k = 0; k < 3; k++)
        sched_run(pol[k], in, quantum, &res[k]);

    char title[64];
    snprintf(title, sizeof title, "Scheduling comparison (same %d jobs, RR quantum = %d)",
             in->count, quantum);
    if (g_verbose) {
        print_header(title);
    } else {
        printf("  %s\n", title);
    }

    printf("  %-28s %-8s %-8s %-8s %-9s %-6s %-10s\n",
           "Policy", "AvgTAT", "AvgWT", "AvgRT", "Makespan", "CPU%", "Dispatches");
    printf("  %-28s %-8s %-8s %-8s %-9s %-6s %-10s\n",
           "----------------------------", "------", "------", "------", "--------", "----", "----------");
    int best = 0, best_rt = 0;
    for (int k = 0; k < 3; k++) {
        char name[40];
        if (pol[k] == POLICY_RR)
            snprintf(name, sizeof name, "Round Robin (q=%d)", quantum);
        else
            snprintf(name, sizeof name, "%s", policy_name(pol[k]));
        printf("  %-28s %-8.2f %-8.2f %-8.2f %-9d %-6.1f %-10d\n", name,
               res[k].avg_tat, res[k].avg_wt, res[k].avg_rt, res[k].makespan,
               100.0 * res[k].busy_time / res[k].makespan, res[k].dispatches);
        if (res[k].avg_wt < res[best].avg_wt)
            best = k;
        if (res[k].avg_rt < res[best_rt].avg_rt)
            best_rt = k;
    }

    if (g_verbose) {
        printf("\n  Per-job waiting time:\n");
        printf("  %-5s %-6s %-6s %-6s\n", "Job", "FCFS", "RR", "PRIO");
        for (int i = 0; i < in->count; i++)
            printf("  J%-4d %-6d %-6d %-6d\n", in->jobs[i].id, res[0].set.jobs[i].waiting,
                   res[1].set.jobs[i].waiting, res[2].set.jobs[i].waiting);
        printf("\n  Execution order:\n");
        for (int k = 0; k < 3; k++) {
            printf("  %-6s: ", pol[k] == POLICY_FCFS ? "FCFS" : pol[k] == POLICY_RR ? "RR" : "PRIO");
            int first = 1;
            for (int s = 0; s < res[k].nslices; s++) {
                if (res[k].slices[s].job_id == IDLE_SLICE) continue;
                printf("%sJ%d", first ? "" : " ", res[k].slices[s].job_id);
                first = 0;
            }
            printf("\n");
        }
    }
    printf("  Lowest average waiting time: %s (%.2f)\n",
           pol[best] == POLICY_RR ? "Round Robin" : policy_name(pol[best]), res[best].avg_wt);
    printf("  Lowest average response time: %s (%.2f)\n",
           pol[best_rt] == POLICY_RR ? "Round Robin" : policy_name(pol[best_rt]),
           res[best_rt].avg_rt);
    logger_log("SCHED", "comparison done: lowest avgWT = %s", policy_name(pol[best]));
    return 0;
}

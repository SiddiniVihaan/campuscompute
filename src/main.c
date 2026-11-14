/*
 * CampusCompute - command-line front end.
 * Every feature is a separate sub-command so it can be tested on its own.
 */
#include "banker.h"
#include "common.h"
#include "ipc.h"
#include "jobs.h"
#include "logger.h"
#include "sched.h"
#include "workers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_QUANTUM   2
#define DEFAULT_JOBS      "tests/jobs.txt"
#define DEFAULT_SAFE      "tests/banker_safe.txt"
#define DEFAULT_UNSAFE    "tests/banker_unsafe.txt"

typedef struct {
    const char *name;
    const char *args;
    const char *help;
    int min_args, max_args;
    int (*fn)(int argc, char **argv);
    int own_logger;      /* command starts/stops the logger itself */
} Command;

static const Command *find_command(const char *name);
static void usage(FILE *out);

static int cmd_usage_error(const char *name)
{
    const Command *c = find_command(name);
    fprintf(stderr, "usage: ./campus %s %s\n", c->name, c->args);
    return EXIT_USAGE;
}

static int get_quantum(const char *s, int *q)
{
    if (parse_int(s, q) != 0 || *q < 1 || *q > 100) {
        fprintf(stderr, "error: time quantum '%s' must be an integer 1..100\n", s);
        return -1;
    }
    return 0;
}

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    usage(stdout);
    return EXIT_OK;
}

static int cmd_jobs(int argc, char **argv)
{
    (void)argc;
    JobSet set;
    if (jobs_load(argv[0], &set) != 0)
        return EXIT_RUNTIME;
    print_header("Job table");
    printf("File: %s\n", argv[0]);
    jobs_print(&set);
    logger_log("JOBS", "validated %d jobs from %s", set.count, argv[0]);
    return EXIT_OK;
}

static int run_single_policy(Policy p, int argc, char **argv)
{
    int q = DEFAULT_QUANTUM;
    if (p == POLICY_RR && argc == 2 && get_quantum(argv[1], &q) != 0)
        return EXIT_USAGE;
    JobSet set;
    if (jobs_load(argv[0], &set) != 0)
        return EXIT_RUNTIME;
    static SchedResult r;
    sched_run(p, &set, q, &r);
    sched_print(&r);
    if (p == POLICY_RR && argc < 2)
        printf("  (quantum %d is the default; pass a number to change it)\n", q);
    return EXIT_OK;
}

static int cmd_fcfs(int argc, char **argv)     { return run_single_policy(POLICY_FCFS, argc, argv); }
static int cmd_rr(int argc, char **argv)       { return run_single_policy(POLICY_RR, argc, argv); }
static int cmd_priority(int argc, char **argv) { return run_single_policy(POLICY_PRIORITY, argc, argv); }

static int cmd_compare(int argc, char **argv)
{
    int q = DEFAULT_QUANTUM;
    if (argc == 2 && get_quantum(argv[1], &q) != 0)
        return EXIT_USAGE;
    JobSet set;
    if (jobs_load(argv[0], &set) != 0)
        return EXIT_RUNTIME;
    return sched_compare(&set, q);
}

static int cmd_logger(int argc, char **argv)
{
    int v[4] = { 3, 2, 6, 4 };
    const char *names[4] = { "producers", "consumers", "events", "capacity" };
    for (int i = 0; i < argc; i++)
        if (parse_int(argv[i], &v[i]) != 0) {
            fprintf(stderr, "error: %s '%s' is not an integer\n", names[i], argv[i]);
            return cmd_usage_error("logger");
        }
    return logger_demo(v[0], v[1], v[2], v[3]);
}

static int cmd_ipc(int argc, char **argv)
{
    JobSet set;
    if (jobs_load(argc ? argv[0] : DEFAULT_JOBS, &set) != 0)
        return EXIT_RUNTIME;
    int n = 0;
    return ipc_run(&set, &n);
}

static int cmd_banker(int argc, char **argv)
{
    (void)argc;
    return banker_run(argv[0], NULL, NULL, 0);
}

static int cmd_workers(int argc, char **argv)
{
    int n = 4, fail = 0;
    if (argc >= 1 && parse_int(argv[0], &n) != 0) {
        fprintf(stderr, "error: worker count '%s' is not an integer\n", argv[0]);
        return cmd_usage_error("workers");
    }
    if (argc == 2) {
        if (strcmp(argv[1], "fail") != 0) {
            fprintf(stderr, "error: unknown option '%s' (expected 'fail')\n", argv[1]);
            return cmd_usage_error("workers");
        }
        fail = 1;
    }
    WorkerSummary s;
    return workers_run(n, fail, &s);
}

static int cmd_rlimit(int argc, char **argv)
{
    return rlimit_run(argc ? argv[0] : "all", NULL);
}

static int cmd_verify(int argc, char **argv)
{
    (void)argc; (void)argv;
    return verify_cleanup();
}

static int cmd_cleanup(int argc, char **argv)
{
    (void)argc; (void)argv;
    return ipc_cleanup_stale();
}

/* Integrated run: every module in sequence, quiet output, then a summary table. */
static int cmd_demo(int argc, char **argv)
{
    const char *jobfile = argc ? argv[0] : DEFAULT_JOBS;
    char row[10][2][128];
    int pass[10], n = 0;

    printf("\n=== CampusCompute integrated demonstration (pid %d) ===\n", (int)getpid());
    logger_log("DEMO", "integrated demo started with %s", jobfile);
    g_verbose = 0;

    JobSet set;
    printf("\n[1/8] Load and validate jobs from %s\n", jobfile);
    if (jobs_load(jobfile, &set) != 0) {
        g_verbose = 1;
        return EXIT_RUNTIME;
    }
    printf("  %d jobs validated\n", set.count);
    snprintf(row[n][0], 128, "Job management (FR1)");
    snprintf(row[n][1], 128, "%d jobs validated", set.count);
    pass[n++] = 1;

    printf("\n[2/8] CPU scheduling: FCFS vs Round Robin vs Priority on the same jobs\n");
    sched_compare(&set, DEFAULT_QUANTUM);
    snprintf(row[n][0], 128, "CPU scheduling (FR2)");
    snprintf(row[n][1], 128, "3 policies compared, q=%d", DEFAULT_QUANTUM);
    pass[n++] = 1;

    int safe = 0;
    char seq[96];
    printf("\n[3/8] Banker's algorithm on %s\n", DEFAULT_SAFE);
    int rc = banker_run(DEFAULT_SAFE, &safe, seq, sizeof seq);
    snprintf(row[n][0], 128, "Banker safe case (FR5)");
    snprintf(row[n][1], 128, "%s <%s>", safe ? "SAFE" : "UNSAFE", seq);
    pass[n++] = rc == EXIT_OK && safe;

    printf("\n[4/8] Banker's algorithm on %s\n", DEFAULT_UNSAFE);
    rc = banker_run(DEFAULT_UNSAFE, &safe, seq, sizeof seq);
    snprintf(row[n][0], 128, "Banker unsafe case (FR5)");
    snprintf(row[n][1], 128, "%s detected", safe ? "SAFE (unexpected)" : "UNSAFE");
    pass[n++] = rc == EXIT_OK && !safe;

    printf("\n[5/8] Dispatch jobs to a worker through shared memory + named semaphores\n");
    int exchanged = 0;
    rc = ipc_run(&set, &exchanged);
    printf("  %d/%d jobs exchanged, IPC objects released: %s\n", exchanged, set.count,
           rc == EXIT_OK ? "yes" : "NO");
    snprintf(row[n][0], 128, "Shared-memory IPC (FR4)");
    snprintf(row[n][1], 128, "%d/%d jobs exchanged", exchanged, set.count);
    pass[n++] = rc == EXIT_OK && exchanged == set.count;

    printf("\n[6/8] Worker processes with simulated failures\n");
    WorkerSummary ws;
    rc = workers_run(4, 1, &ws);
    printf("  %d workers: %d completed, %d failed (%d simulated) - detected via waitpid\n",
           ws.total, ws.completed, ws.failed, ws.simulated);
    snprintf(row[n][0], 128, "Failure detection (FR6)");
    snprintf(row[n][1], 128, "%d ok / %d failed detected", ws.completed, ws.failed);
    pass[n++] = rc == EXIT_OK && ws.completed == 2 && ws.failed == 2;

    printf("\n[7/8] Resource limit: RLIMIT_CPU on a runaway worker\n");
    int enforced = 0;
    rlimit_run("cpu", &enforced);
    printf("  runaway child %s\n", enforced ? "stopped by SIGXCPU after 1 s CPU" : "NOT stopped");
    snprintf(row[n][0], 128, "Resource limit (FR7)");
    snprintf(row[n][1], 128, "%s", enforced ? "SIGXCPU enforced" : "not enforced");
    pass[n++] = enforced;

    printf("\n[8/8] Cleanup verification\n");
    rc = verify_cleanup();
    snprintf(row[n][0], 128, "Cleanup (FR7)");
    snprintf(row[n][1], 128, "%s", rc == EXIT_OK ? "no leftovers" : "leftovers found");
    pass[n++] = rc == EXIT_OK;

    logger_log("DEMO", "integrated demo finished");
    LoggerStats ls;
    logger_stop();
    logger_get_stats(&ls);
    snprintf(row[n][0], 128, "Activity logger (FR3)");
    snprintf(row[n][1], 128, "%lu entries written, threads joined", ls.consumed);
    pass[n++] = ls.consumed == ls.produced && ls.dropped == 0;
    g_verbose = 1;

    int all = 1;
    printf("\n=== Integrated demo summary ===\n");
    printf("  %-26s %-36s %s\n", "Stage", "Result", "Status");
    printf("  %-26s %-36s %s\n", "--------------------------", "------------------------------------", "------");
    for (int i = 0; i < n; i++) {
        printf("  %-26s %-36s %s\n", row[i][0], row[i][1], pass[i] ? "PASS" : "FAIL");
        all = all && pass[i];
    }
    printf("\n  Overall: %s (%d stages). Log: ./%s\n", all ? "ALL PASS" : "SOME STAGES FAILED", n, LOG_FILE);
    return all ? EXIT_OK : EXIT_RUNTIME;
}

static const Command commands[] = {
    { "help",     "",                                 "show this help",                                        0, 0, cmd_help,     0 },
    { "jobs",     "<jobfile>",                        "validate and list a job file (FR1)",                    1, 1, cmd_jobs,     0 },
    { "fcfs",     "<jobfile>",                        "First-Come First-Served scheduling (FR2)",              1, 1, cmd_fcfs,     0 },
    { "rr",       "<jobfile> [quantum]",              "Round Robin scheduling, default quantum 2 (FR2)",       1, 2, cmd_rr,       0 },
    { "priority", "<jobfile>",                        "non-preemptive Priority scheduling (FR2)",              1, 1, cmd_priority, 0 },
    { "compare",  "<jobfile> [quantum]",              "compare all three policies on one job set (FR2)",       1, 2, cmd_compare,  0 },
    { "logger",   "[producers consumers events cap]", "pthread producer-consumer logger demo, default 3 2 6 4 (FR3)", 0, 4, cmd_logger, 1 },
    { "ipc",      "[jobfile]",                        "shared memory + named semaphore dispatch (FR4)",        0, 1, cmd_ipc,      0 },
    { "banker",   "<statefile>",                      "Banker's safety algorithm (FR5)",                       1, 1, cmd_banker,   0 },
    { "workers",  "[count] [fail]",                   "fork/waitpid workers; 'fail' adds simulated failures (FR6)", 0, 2, cmd_workers, 0 },
    { "rlimit",   "[cpu|mem|all]",                    "setrlimit demonstration in child processes (FR7)",     0, 1, cmd_rlimit,   0 },
    { "verify",   "",                                 "check no IPC objects / worker processes are left (FR7)", 0, 0, cmd_verify,  0 },
    { "cleanup",  "",                                 "remove stale campus_* IPC objects from crashed runs",   0, 0, cmd_cleanup,  0 },
    { "demo",     "[jobfile]",                        "integrated run of every module with a summary",         0, 1, cmd_demo,     0 },
};
#define NCOMMANDS ((int)(sizeof commands / sizeof commands[0]))

static const Command *find_command(const char *name)
{
    for (int i = 0; i < NCOMMANDS; i++)
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    return NULL;
}

static void usage(FILE *out)
{
    fprintf(out, "CampusCompute - job scheduling and resource management prototype\n\n");
    fprintf(out, "Usage: ./campus <command> [arguments]\n\nCommands:\n");
    for (int i = 0; i < NCOMMANDS; i++) {
        char left[64];
        snprintf(left, sizeof left, "%s %s", commands[i].name, commands[i].args);
        fprintf(out, "  %-42s %s\n", left, commands[i].help);
    }
    fprintf(out, "\nJob file line format : id arrival burst priority   ('#' starts a comment)\n");
    fprintf(out, "Priority             : lower number = higher priority\n");
    fprintf(out, "Activity log         : ./%s\n", LOG_FILE);
    fprintf(out, "\nExamples:\n");
    fprintf(out, "  ./campus rr tests/jobs.txt 3\n");
    fprintf(out, "  ./campus banker tests/banker_safe.txt\n");
    fprintf(out, "  ./campus workers 4 fail\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "error: no command given\n\n");
        usage(stderr);
        return EXIT_USAGE;
    }
    const Command *c = find_command(argv[1]);
    if (c == NULL) {
        fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
        fprintf(stderr, "Run './campus help' to see the available commands.\n");
        if (logger_start(LOG_FILE, 4, 1, 0, 0) == 0) {
            logger_log("MAIN", "rejected unknown command '%s'", argv[1]);
            logger_stop();
        }
        return EXIT_USAGE;
    }
    int nargs = argc - 2;
    if (nargs < c->min_args || nargs > c->max_args) {
        if (c->min_args == c->max_args)
            fprintf(stderr, "error: '%s' expects %d argument(s), got %d\n",
                    c->name, c->min_args, nargs);
        else
            fprintf(stderr, "error: '%s' expects %d to %d argument(s), got %d\n",
                    c->name, c->min_args, c->max_args, nargs);
        return cmd_usage_error(c->name);
    }

    if (!c->own_logger && logger_start(LOG_FILE, 16, 1, 0, 0) != 0)
        fprintf(stderr, "warning: activity logger unavailable, continuing without it\n");
    logger_log("MAIN", "pid %d command: %s", (int)getpid(), c->name);

    int rc = c->fn(nargs, argv + 2);

    logger_log("MAIN", "pid %d command %s finished with exit code %d", (int)getpid(), c->name, rc);
    logger_stop();
    return rc;
}

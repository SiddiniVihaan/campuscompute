/*
 * Worker processes (fork + waitpid) and resource limits (setrlimit).
 *
 * Failures here are SIMULATED on purpose: a worker either exits with the
 * reserved code SIM_FAIL_EXIT or sends itself SIGTERM. They are labelled as
 * simulated in the output and are not real OS faults.
 */
#include "workers.h"
#include "common.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { W_NORMAL, W_SIM_EXIT, W_SIM_CRASH } WorkerMode;

static const char *mode_name(WorkerMode m)
{
    switch (m) {
    case W_NORMAL:    return "normal job";
    case W_SIM_EXIT:  return "SIMULATED FAILURE (exit code 42)";
    case W_SIM_CRASH: return "SIMULATED CRASH (SIGTERM to self)";
    }
    return "?";
}

static void worker_child(int id, WorkerMode mode)
{
    pid_t me = getpid();
    INFO("  [worker W%d pid %d] started, parent pid %d, mode: %s\n",
         id, (int)me, (int)getppid(), mode_name(mode));
    fflush(stdout);

    /* Staggered simulated work so workers finish in a different order than created. */
    sleep_ms(100 + ((id * 7) % 5) * 80);

    switch (mode) {
    case W_NORMAL:
        INFO("  [worker W%d pid %d] job finished, exiting with status 0\n", id, (int)me);
        fflush(stdout);
        _exit(0);
    case W_SIM_EXIT:
        INFO("  [worker W%d pid %d] SIMULATED FAILURE: deliberately exiting with code %d "
             "(not a real OS error)\n", id, (int)me, SIM_FAIL_EXIT);
        fflush(stdout);
        _exit(SIM_FAIL_EXIT);
    case W_SIM_CRASH:
        INFO("  [worker W%d pid %d] SIMULATED CRASH: deliberately sending SIGTERM to itself "
             "(not a real OS error)\n", id, (int)me);
        fflush(stdout);
        raise(SIGTERM);
        _exit(99);   /* not reached */
    }
}

int workers_run(int count, int with_failures, WorkerSummary *sum)
{
    if (count < 1 || count > MAX_WORKERS) {
        fprintf(stderr, "error: worker count must be 1..%d\n", MAX_WORKERS);
        return EXIT_USAGE;
    }
    if (with_failures && count < 3) {
        fprintf(stderr, "error: 'fail' mode needs at least 3 workers (W2 fails, W3 crashes)\n");
        return EXIT_USAGE;
    }

    print_header(with_failures ? "Worker processes with SIMULATED failures"
                               : "Worker processes (normal completion)");
    INFO("Parent pid %d creating %d worker(s) with fork(); results collected with waitpid().\n",
         (int)getpid(), count);
    if (with_failures)
        INFO("Simulation plan: W2 exits with code %d, W3 sends itself SIGTERM; others succeed.\n",
             SIM_FAIL_EXIT);
    INFO("\n");

    pid_t pids[MAX_WORKERS];
    double started[MAX_WORKERS];
    int created = 0;
    memset(sum, 0, sizeof *sum);

    for (int i = 0; i < count; i++) {
        WorkerMode mode = W_NORMAL;
        if (with_failures && i == 1) mode = W_SIM_EXIT;
        if (with_failures && i == 2) mode = W_SIM_CRASH;

        fflush(stdout);
        pid_t pid = fork();
        if (pid == -1) {
            fprintf(stderr, "error: fork: %s\n", strerror(errno));
            break;
        }
        if (pid == 0)
            worker_child(i + 1, mode);
        pids[i] = pid;
        started[i] = now_seconds();
        created++;
        logger_log("WORKER", "created W%d pid %d (%s)", i + 1, (int)pid, mode_name(mode));
    }

    /* Reap in completion order; the parent decides success purely from the wait status. */
    int reaped = 0;
    while (reaped < created) {
        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "error: waitpid: %s\n", strerror(errno));
            break;
        }
        int idx = -1;
        for (int i = 0; i < created; i++)
            if (pids[i] == pid) idx = i;
        if (idx < 0)
            continue;
        reaped++;
        double ms = (now_seconds() - started[idx]) * 1000.0;
        char verdict[160];

        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            snprintf(verdict, sizeof verdict, "COMPLETED normally (exit status 0)");
            sum->completed++;
        } else if (WIFEXITED(st)) {
            int code = WEXITSTATUS(st);
            snprintf(verdict, sizeof verdict, "FAILED: exit status %d%s", code,
                     code == SIM_FAIL_EXIT ? " [simulated failure]" : "");
            sum->failed++;
            if (code == SIM_FAIL_EXIT) sum->simulated++;
        } else if (WIFSIGNALED(st)) {
            int sig = WTERMSIG(st);
            snprintf(verdict, sizeof verdict, "FAILED: terminated by signal %d (%s)%s", sig,
                     strsignal(sig), sig == SIGTERM ? " [simulated crash]" : "");
            sum->failed++;
            if (sig == SIGTERM) sum->simulated++;
        } else {
            snprintf(verdict, sizeof verdict, "unknown status 0x%x", st);
            sum->failed++;
        }
        INFO("  [parent] waitpid -> W%d pid %d after %.0f ms: %s\n",
             idx + 1, (int)pid, ms, verdict);
        logger_log("WORKER", "W%d pid %d: %s", idx + 1, (int)pid, verdict);
    }
    sum->total = created;

    int leftover = waitpid(-1, NULL, WNOHANG);
    INFO("\nSummary: %d worker(s): %d completed, %d failed (%d of them simulated).\n",
         created, sum->completed, sum->failed, sum->simulated);
    INFO("All children reaped: %s\n",
         (leftover == -1 && errno == ECHILD) ? "yes (waitpid reports ECHILD, no zombies)" : "NO");
    return created == count ? EXIT_OK : EXIT_RUNTIME;
}

/* ------------------------------ resource limits ------------------------------ */

static void print_limit(const char *who, const char *name, int resource, const char *unit)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) != 0) {
        printf("  %s %s: getrlimit failed: %s\n", who, name, strerror(errno));
        return;
    }
    char soft[32], hard[32];
    if (rl.rlim_cur == RLIM_INFINITY) snprintf(soft, sizeof soft, "unlimited");
    else snprintf(soft, sizeof soft, "%llu%s", (unsigned long long)rl.rlim_cur, unit);
    if (rl.rlim_max == RLIM_INFINITY) snprintf(hard, sizeof hard, "unlimited");
    else snprintf(hard, sizeof hard, "%llu%s", (unsigned long long)rl.rlim_max, unit);
    printf("  %s %s: soft=%s hard=%s\n", who, name, soft, hard);
}

static long vm_size_kb(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (fp == NULL) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, fp) != NULL)
        if (sscanf(line, "VmSize: %ld kB", &kb) == 1) break;
    fclose(fp);
    return kb;
}

static int run_cpu_limit(void)
{
    INFO("\n--- RLIMIT_CPU: limit a CPU-bound worker to 1 second of CPU time ---\n");
    fflush(stdout);
    double t0 = now_seconds();
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "error: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        struct rlimit rl = { 1, 2 };   /* soft 1 s -> SIGXCPU, hard 2 s -> SIGKILL */
        if (setrlimit(RLIMIT_CPU, &rl) != 0) {
            fprintf(stderr, "  [child] setrlimit(RLIMIT_CPU): %s\n", strerror(errno));
            _exit(3);
        }
        if (g_verbose) print_limit("[child]", "RLIMIT_CPU", RLIMIT_CPU, "s");
        INFO("  [child pid %d] entering infinite busy loop...\n", (int)getpid());
        fflush(stdout);
        alarm(10);                     /* safety net if the limit is not enforced */
        volatile unsigned long x = 0;
        for (;;) x++;
    }

    int st;
    struct rusage ru;
    while (wait4(pid, &st, 0, &ru) == -1 && errno == EINTR)
        ;
    double wall = now_seconds() - t0;
    double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                 ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    int enforced = WIFSIGNALED(st) && WTERMSIG(st) == SIGXCPU;
    if (WIFSIGNALED(st))
        INFO("  [parent] child pid %d terminated by signal %d (%s)\n",
             (int)pid, WTERMSIG(st), strsignal(WTERMSIG(st)));
    else
        INFO("  [parent] child pid %d exited with status %d\n", (int)pid, WEXITSTATUS(st));
    INFO("  [parent] child CPU time used: %.2f s, wall time: %.2f s\n", cpu, wall);
    INFO("  Observed effect: %s\n", enforced
         ? "kernel sent SIGXCPU once the 1 s CPU soft limit was reached -> runaway job stopped"
         : "limit NOT enforced as expected");
    if (g_verbose) print_limit("[parent]", "RLIMIT_CPU", RLIMIT_CPU, "s");
    INFO("  (parent limit unchanged: setrlimit only affected the child)\n");
    logger_log("RLIMIT", "RLIMIT_CPU child pid %d: cpu=%.2fs %s", (int)pid, cpu,
               enforced ? "stopped by SIGXCPU" : "not enforced");
    return enforced;
}

static int run_mem_limit(void)
{
    INFO("\n--- RLIMIT_AS: cap a worker's virtual address space ---\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "error: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        long cur_kb = vm_size_kb();
        rlim_t limit = (rlim_t)(cur_kb > 0 ? cur_kb : 65536) * 1024 + 32UL * 1024 * 1024;
        struct rlimit rl = { limit, limit };
        if (setrlimit(RLIMIT_AS, &rl) != 0) {
            fprintf(stderr, "  [child] setrlimit(RLIMIT_AS): %s\n", strerror(errno));
            _exit(3);
        }
        INFO("  [child pid %d] current size %ld KB, RLIMIT_AS set to %llu MB (current + 32 MB)\n",
             (int)getpid(), cur_kb, (unsigned long long)(limit >> 20));

        size_t big = 256UL * 1024 * 1024;
        errno = 0;
        char *p = malloc(big);
        int big_failed = (p == NULL);
        INFO("  [child] malloc(256 MB) -> %s\n",
             big_failed ? "NULL: Cannot allocate memory (ENOMEM)" : "succeeded (limit not hit)");
        free(p);

        size_t small = 8UL * 1024 * 1024;
        char *s = malloc(small);
        if (s) memset(s, 1, small);
        INFO("  [child] malloc(8 MB) within the limit -> %s\n", s ? "succeeded" : "failed");
        free(s);
        fflush(stdout);
        _exit(big_failed && s ? 0 : 1);
    }
    int st;
    while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
        ;
    int enforced = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    INFO("  [parent] child pid %d exited with status %d\n", (int)pid,
         WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    INFO("  Observed effect: %s\n", enforced
         ? "large allocation refused by the kernel, small one allowed -> memory capped"
         : "limit NOT enforced as expected");
    if (g_verbose) print_limit("[parent]", "RLIMIT_AS", RLIMIT_AS, " bytes");
    logger_log("RLIMIT", "RLIMIT_AS child pid %d: %s", (int)pid,
               enforced ? "256MB malloc refused" : "not enforced");
    return enforced;
}

int rlimit_run(const char *which, int *enforced)
{
    int do_cpu = strcmp(which, "cpu") == 0 || strcmp(which, "all") == 0;
    int do_mem = strcmp(which, "mem") == 0 || strcmp(which, "all") == 0;
    if (!do_cpu && !do_mem) {
        fprintf(stderr, "error: rlimit: unknown limit '%s' (use cpu, mem or all)\n", which);
        return EXIT_USAGE;
    }
    print_header("Process resource limits (setrlimit)");
    INFO("Limits are applied inside forked children so the main program is unaffected.\n");
    int ok = 1, r;
    if (do_cpu) {
        r = run_cpu_limit();
        ok = ok && r == 1;
    }
    if (do_mem) {
        r = run_mem_limit();
        ok = ok && r == 1;
    }
    if (enforced) *enforced = ok;
    INFO("\nResult: %s\n", ok ? "all requested limits enforced" : "a limit was not enforced");
    return ok ? EXIT_OK : EXIT_RUNTIME;
}

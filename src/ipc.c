/*
 * Shared-memory IPC between a dispatcher (parent) and a worker (child).
 *
 * One POSIX shared-memory object holds a single-slot "mailbox". Two named
 * POSIX semaphores enforce strict alternation:
 *     sem_req  (starts 0): parent posts after writing a request
 *     sem_rep  (starts 0): child  posts after writing the reply
 * Because only one side may touch the mailbox at a time, no extra mutex is needed.
 *
 * Object names include the parent PID, so we never collide with or touch
 * objects that belong to anyone else.
 */
#include "ipc.h"
#include "common.h"
#include "logger.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IPC_TIMEOUT_SEC 5

typedef struct {
    int shutdown;
    int job_id;
    int burst;
    int reply_job_id;
    pid_t worker_pid;
    long result;
    long iterations;
} Mailbox;

/* Waits on a semaphore with a timeout so a dead peer cannot hang us forever. */
static int sem_wait_timeout(sem_t *s, int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    int rc;
    while ((rc = sem_timedwait(s, &ts)) == -1 && errno == EINTR)
        ;
    return rc;
}

/* Simulated CPU work: a checksum loop proportional to the job's burst. */
static long simulate_work(int burst, long *iterations)
{
    long n = (long)burst * 200000L;
    unsigned long acc = 1469598103UL;
    for (long i = 0; i < n; i++)
        acc = (acc ^ (unsigned long)i) * 1099511UL % 1000000007UL;
    *iterations = n;
    return (long)acc;
}

static void worker_loop(Mailbox *box, sem_t *req, sem_t *rep)
{
    for (;;) {
        if (sem_wait_timeout(req, IPC_TIMEOUT_SEC) != 0) {
            fprintf(stderr, "  [worker pid %d] timed out waiting for dispatcher\n", getpid());
            _exit(2);
        }
        if (box->shutdown) {
            INFO("  [worker pid %d] read SHUTDOWN from shared memory -> exiting\n", getpid());
            fflush(stdout);
            break;
        }
        long iters = 0;
        long value = simulate_work(box->burst, &iters);
        box->reply_job_id = box->job_id;
        box->worker_pid = getpid();
        box->result = value;
        box->iterations = iters;
        INFO("  [worker pid %d] read job J%d (burst %d) from shared memory, "
             "computed checksum %ld, wrote reply\n", getpid(), box->job_id, box->burst, value);
        fflush(stdout);
        sem_post(rep);
    }
}

int ipc_run(const JobSet *jobs, int *exchanged)
{
    char shm_name[64], req_name[64], rep_name[64];
    pid_t me = getpid();
    snprintf(shm_name, sizeof shm_name, "/%sshm_%d", IPC_PREFIX, (int)me);
    snprintf(req_name, sizeof req_name, "/%sreq_%d", IPC_PREFIX, (int)me);
    snprintf(rep_name, sizeof rep_name, "/%srep_%d", IPC_PREFIX, (int)me);

    int status = EXIT_RUNTIME, fd = -1, done = 0;
    Mailbox *box = MAP_FAILED;
    sem_t *req = SEM_FAILED, *rep = SEM_FAILED;
    pid_t child = -1;
    *exchanged = 0;

    print_header("Shared-memory IPC with named semaphores");

    fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        fprintf(stderr, "error: shm_open(%s): %s%s\n", shm_name, strerror(errno),
                errno == EEXIST ? " (stale object? run: ./campus cleanup)" : "");
        goto cleanup;
    }
    if (ftruncate(fd, sizeof(Mailbox)) == -1) {
        fprintf(stderr, "error: ftruncate: %s\n", strerror(errno));
        goto cleanup;
    }
    box = mmap(NULL, sizeof(Mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (box == MAP_FAILED) {
        fprintf(stderr, "error: mmap: %s\n", strerror(errno));
        goto cleanup;
    }
    close(fd);
    fd = -1;
    memset(box, 0, sizeof *box);

    req = sem_open(req_name, O_CREAT | O_EXCL, 0600, 0);
    if (req == SEM_FAILED) {
        fprintf(stderr, "error: sem_open(%s): %s\n", req_name, strerror(errno));
        goto cleanup;
    }
    rep = sem_open(rep_name, O_CREAT | O_EXCL, 0600, 0);
    if (rep == SEM_FAILED) {
        fprintf(stderr, "error: sem_open(%s): %s\n", rep_name, strerror(errno));
        goto cleanup;
    }

    INFO("Created shared memory : %s (%zu bytes, visible as /dev/shm%s)\n",
         shm_name, sizeof(Mailbox), shm_name);
    INFO("Created semaphores    : %s (request), %s (reply), both initial value 0\n",
         req_name, rep_name);
    INFO("Protocol: parent writes job -> sem_post(req) -> child reads, computes, writes\n"
         "          reply -> sem_post(rep) -> parent reads reply. Repeat; then SHUTDOWN.\n\n");
    logger_log("IPC", "created %s, %s, %s", shm_name, req_name, rep_name);

    fflush(stdout);     /* avoid the child inheriting and re-printing buffered output */
    child = fork();
    if (child == -1) {
        fprintf(stderr, "error: fork: %s\n", strerror(errno));
        goto cleanup;
    }
    if (child == 0) {
        worker_loop(box, req, rep);
        munmap(box, sizeof *box);
        sem_close(req);
        sem_close(rep);
        _exit(0);
    }

    INFO("  [dispatcher pid %d] forked worker pid %d\n", (int)me, (int)child);
    for (int i = 0; i < jobs->count; i++) {
        const Job *j = &jobs->jobs[i];
        box->job_id = j->id;
        box->burst = j->burst;
        INFO("  [dispatcher pid %d] wrote job J%d (burst %d) into shared memory, sem_post(req)\n",
             (int)me, j->id, j->burst);
        fflush(stdout);
        sem_post(req);
        if (sem_wait_timeout(rep, IPC_TIMEOUT_SEC) != 0) {
            fprintf(stderr, "error: no reply from worker within %d s\n", IPC_TIMEOUT_SEC);
            kill(child, SIGTERM);
            goto reap;
        }
        if (box->reply_job_id != j->id) {
            fprintf(stderr, "error: reply mismatch (expected J%d got J%d)\n", j->id, box->reply_job_id);
            kill(child, SIGTERM);
            goto reap;
        }
        INFO("  [dispatcher pid %d] sem_wait(rep) done, reply for J%d from pid %d: checksum=%ld (%ld iterations)\n",
             (int)me, box->reply_job_id, (int)box->worker_pid, box->result, box->iterations);
        logger_log("IPC", "job J%d processed by worker pid %d via shared memory, checksum=%ld",
                   j->id, (int)box->worker_pid, box->result);
        done++;
    }
    box->shutdown = 1;
    INFO("  [dispatcher pid %d] wrote SHUTDOWN flag, sem_post(req)\n", (int)me);
    fflush(stdout);
    sem_post(req);
    status = EXIT_OK;

reap:;
    int wst = 0;
    while (waitpid(child, &wst, 0) == -1 && errno == EINTR)
        ;
    if (WIFEXITED(wst))
        INFO("  [dispatcher pid %d] worker pid %d exited with status %d\n",
             (int)me, (int)child, WEXITSTATUS(wst));
    else if (WIFSIGNALED(wst))
        INFO("  [dispatcher pid %d] worker pid %d killed by signal %d\n",
             (int)me, (int)child, WTERMSIG(wst));
    if (!(WIFEXITED(wst) && WEXITSTATUS(wst) == 0))
        status = EXIT_RUNTIME;

cleanup:
    if (fd != -1) close(fd);
    if (box != MAP_FAILED) munmap(box, sizeof *box);
    if (req != SEM_FAILED) { sem_close(req); sem_unlink(req_name); }
    if (rep != SEM_FAILED) { sem_close(rep); sem_unlink(rep_name); }
    shm_unlink(shm_name);     /* harmless ENOENT if creation failed */

    char path[128];
    int left = 0;
    snprintf(path, sizeof path, "/dev/shm%s", shm_name);
    left += access(path, F_OK) == 0;
    snprintf(path, sizeof path, "/dev/shm/sem.%s", req_name + 1);
    left += access(path, F_OK) == 0;
    snprintf(path, sizeof path, "/dev/shm/sem.%s", rep_name + 1);
    left += access(path, F_OK) == 0;

    INFO("\nCleanup: munmap + shm_unlink(%s), sem_close + sem_unlink(%s, %s)\n",
         shm_name, req_name, rep_name);
    INFO("Cleanup check: %d of this run's IPC objects remain in /dev/shm -> %s\n",
         left, left == 0 ? "all released" : "LEAK");
    INFO("Jobs exchanged through shared memory: %d/%d\n", done, jobs->count);
    logger_log("IPC", "cleanup done, %d objects left, %d/%d jobs exchanged", left, done, jobs->count);
    *exchanged = done;
    if (left != 0)
        status = EXIT_RUNTIME;
    return status;
}

/* Lists (and counts) IPC objects in /dev/shm that carry this program's prefix. */
int ipc_count_objects(int print)
{
    DIR *d = opendir("/dev/shm");
    if (d == NULL) {
        fprintf(stderr, "error: opendir(/dev/shm): %s\n", strerror(errno));
        return -1;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, IPC_PREFIX, strlen(IPC_PREFIX)) == 0 ||
            strncmp(e->d_name, "sem." IPC_PREFIX, strlen("sem." IPC_PREFIX)) == 0) {
            if (print) printf("  /dev/shm/%s\n", e->d_name);
            count++;
        }
    }
    closedir(d);
    return count;
}

/* Removes leftovers from crashed/interrupted runs - only objects with our prefix. */
int ipc_cleanup_stale(void)
{
    print_header("Cleanup of stale CampusCompute IPC objects");
    DIR *d = opendir("/dev/shm");
    if (d == NULL) {
        fprintf(stderr, "error: opendir(/dev/shm): %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }
    int removed = 0, failed = 0;
    struct dirent *e;
    char name[300];
    while ((e = readdir(d)) != NULL) {
        int rc = 1;
        if (strncmp(e->d_name, IPC_PREFIX, strlen(IPC_PREFIX)) == 0) {
            snprintf(name, sizeof name, "/%s", e->d_name);
            rc = shm_unlink(name);
        } else if (strncmp(e->d_name, "sem." IPC_PREFIX, strlen("sem." IPC_PREFIX)) == 0) {
            snprintf(name, sizeof name, "/%s", e->d_name + 4);
            rc = sem_unlink(name);
        }
        if (rc == 0) {
            printf("  removed %s\n", name);
            removed++;
        } else if (rc == -1) {
            printf("  could not remove %s: %s\n", name, strerror(errno));
            failed++;
        }
    }
    closedir(d);
    printf("Removed %d object(s) with prefix '%s' (other files in /dev/shm untouched).\n",
           removed, IPC_PREFIX);
    logger_log("CLEAN", "stale cleanup removed %d objects", removed);
    return failed ? EXIT_RUNTIME : EXIT_OK;
}

/* Counts other running processes named "campus" (e.g. orphaned workers). */
static int count_campus_processes(void)
{
    DIR *d = opendir("/proc");
    if (d == NULL)
        return -1;
    int count = 0;
    pid_t self = getpid();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid;
        if (parse_int(e->d_name, &pid) != 0 || pid == self)
            continue;
        char path[300], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        FILE *fp = fopen(path, "r");
        if (fp == NULL)
            continue;
        if (fgets(comm, sizeof comm, fp) != NULL) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, "campus") == 0) {
                printf("  still running: pid %d (%s)\n", pid, comm);
                count++;
            }
        }
        fclose(fp);
    }
    closedir(d);
    return count;
}

int verify_cleanup(void)
{
    if (g_verbose)
        print_header("Cleanup verification");
    INFO("CampusCompute IPC objects in /dev/shm:\n");
    int objs = ipc_count_objects(g_verbose);
    INFO("  -> %d object(s)\n", objs);
    INFO("Other 'campus' processes (workers/children) still alive:\n");
    int procs = count_campus_processes();
    INFO("  -> %d process(es)\n", procs);

    int children = 0;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        children++;
    INFO("Unreaped children of this process: %d\n", children);
    INFO("Logger threads: %s\n", logger_running() ? "running (stopped at program exit)" : "none");

    int ok = (objs == 0 && procs == 0 && children == 0);
    printf("  Verification: %s\n", ok ? "PASS - no leftover IPC objects or processes"
                                      : "FAIL - leftovers found (run ./campus cleanup)");
    logger_log("VERIFY", "objects=%d processes=%d -> %s", objs, procs, ok ? "PASS" : "FAIL");
    return ok ? EXIT_OK : EXIT_RUNTIME;
}

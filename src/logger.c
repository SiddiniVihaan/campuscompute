/*
 * Concurrent activity logger: a bounded producer-consumer queue.
 *
 *  - Any thread that calls logger_log() is a producer.
 *  - One or more consumer threads pop entries and write them to the log file.
 *  - A mutex protects the ring buffer; two condition variables signal
 *    "not full" (wakes producers) and "not empty" (wakes consumers).
 *  - Shutdown: logger_stop() sets `closed` and broadcasts both conditions.
 *    Consumers keep draining until the queue is empty, then exit and are joined,
 *    so no entry that was accepted is ever lost.
 */
#include "logger.h"
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONSUMERS 8

typedef struct {
    unsigned long seq;
    char text[LOG_MSG_MAX + 160];
} LogEntry;

typedef struct {
    LogEntry *buf;
    int cap, head, tail, count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    unsigned long produced, consumed, dropped, full_waits, empty_waits;
} BoundedQueue;

static BoundedQueue q;
static FILE *log_fp;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t consumer_tid[MAX_CONSUMERS];
static int n_consumers;
static int consumer_delay;
static int verbose_consumers;
static int running;
static pid_t owner_pid;

static int bq_push(const char *text, int *waited, int *depth)
{
    int w = 0;
    pthread_mutex_lock(&q.lock);
    while (q.count == q.cap && !q.closed) {
        if (!w) q.full_waits++;
        w = 1;
        pthread_cond_wait(&q.not_full, &q.lock);   /* loop guards against spurious wakeups */
    }
    if (q.closed) {
        q.dropped++;
        pthread_mutex_unlock(&q.lock);
        return -1;
    }
    LogEntry *e = &q.buf[q.tail];
    e->seq = ++q.produced;
    snprintf(e->text, sizeof e->text, "#%04lu %s", e->seq, text);
    q.tail = (q.tail + 1) % q.cap;
    q.count++;
    if (depth) *depth = q.count;
    pthread_cond_signal(&q.not_empty);
    pthread_mutex_unlock(&q.lock);
    if (waited) *waited = w;
    return 0;
}

/* Returns 1 with an entry, or 0 once the queue is closed AND fully drained. */
static int bq_pop(LogEntry *out, int *waited, int *depth)
{
    int w = 0;
    pthread_mutex_lock(&q.lock);
    while (q.count == 0 && !q.closed) {
        if (!w) q.empty_waits++;
        w = 1;
        pthread_cond_wait(&q.not_empty, &q.lock);
    }
    if (q.count == 0) {
        pthread_mutex_unlock(&q.lock);
        return 0;
    }
    *out = q.buf[q.head];
    q.head = (q.head + 1) % q.cap;
    q.count--;
    q.consumed++;
    if (depth) *depth = q.count;
    pthread_cond_signal(&q.not_full);
    pthread_mutex_unlock(&q.lock);
    if (waited) *waited = w;
    return 1;
}

static void *consumer_main(void *arg)
{
    int cid = (int)(intptr_t)arg;
    LogEntry e;
    int waited = 0, depth = 0;
    while (bq_pop(&e, &waited, &depth)) {
        if (consumer_delay > 0)
            sleep_ms(consumer_delay);      /* simulates slow storage so the queue fills */
        pthread_mutex_lock(&file_lock);
        fprintf(log_fp, "%s\n", e.text);
        fflush(log_fp);
        pthread_mutex_unlock(&file_lock);
        if (verbose_consumers)
            printf("  [consumer C%d] wrote log #%04lu to %s (queue %d/%d)%s\n", cid, e.seq,
                   LOG_FILE, depth, q.cap, waited ? "  <- had waited: queue was EMPTY" : "");
    }
    if (verbose_consumers)
        printf("  [consumer C%d] queue closed and drained -> thread exiting\n", cid);
    return NULL;
}

int logger_start(const char *path, int capacity, int nconsumers,
                 int consumer_delay_ms, int verbose)
{
    if (running)
        return 0;
    if (capacity < 1 || nconsumers < 1 || nconsumers > MAX_CONSUMERS) {
        fprintf(stderr, "error: logger: invalid capacity/consumer count\n");
        return -1;
    }
    log_fp = fopen(path, "a");
    if (log_fp == NULL) {
        fprintf(stderr, "error: cannot open log file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    memset(&q, 0, sizeof q);
    q.buf = calloc((size_t)capacity, sizeof *q.buf);
    if (q.buf == NULL) {
        fclose(log_fp);
        fprintf(stderr, "error: logger: out of memory\n");
        return -1;
    }
    q.cap = capacity;
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.not_full, NULL);
    pthread_cond_init(&q.not_empty, NULL);
    consumer_delay = consumer_delay_ms;
    verbose_consumers = verbose;
    owner_pid = getpid();

    n_consumers = 0;
    for (int i = 0; i < nconsumers; i++) {
        int rc = pthread_create(&consumer_tid[i], NULL, consumer_main, (void *)(intptr_t)(i + 1));
        if (rc != 0) {
            fprintf(stderr, "error: pthread_create: %s\n", strerror(rc));
            break;
        }
        n_consumers++;
    }
    running = 1;
    if (n_consumers == 0) {
        logger_stop();
        return -1;
    }
    return 0;
}

int logger_running(void)
{
    return running && getpid() == owner_pid;
}

static int logger_vlog(const char *module, int *waited, int *depth,
                       const char *fmt, va_list ap)
{
    /*
     * A forked child inherits a copy of the queue but NOT the consumer threads,
     * so logging from a child could block forever. Only the owning process logs.
     */
    if (!logger_running())
        return -1;

    char msg[LOG_MSG_MAX];
    vsnprintf(msg, sizeof msg, fmt, ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    char line[LOG_MSG_MAX + 128];
    snprintf(line, sizeof line, "%s.%03ld [%-6s] %s", stamp, ts.tv_nsec / 1000000L, module, msg);
    return bq_push(line, waited, depth);
}

int logger_log(const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = logger_vlog(module, NULL, NULL, fmt, ap);
    va_end(ap);
    return rc;
}

int logger_log_ex(const char *module, int *waited, int *depth, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = logger_vlog(module, waited, depth, fmt, ap);
    va_end(ap);
    return rc;
}

void logger_get_stats(LoggerStats *out)
{
    int locked = running;            /* after logger_stop() the mutex is destroyed */
    if (locked) pthread_mutex_lock(&q.lock);
    out->produced = q.produced;
    out->consumed = q.consumed;
    out->dropped = q.dropped;
    out->full_waits = q.full_waits;
    out->empty_waits = q.empty_waits;
    out->capacity = q.cap;
    out->consumers = n_consumers;
    if (locked) pthread_mutex_unlock(&q.lock);
}

void logger_stop(void)
{
    if (!running || getpid() != owner_pid)
        return;
    pthread_mutex_lock(&q.lock);
    q.closed = 1;
    pthread_cond_broadcast(&q.not_empty);
    pthread_cond_broadcast(&q.not_full);
    pthread_mutex_unlock(&q.lock);

    for (int i = 0; i < n_consumers; i++)
        pthread_join(consumer_tid[i], NULL);
    running = 0;

    fclose(log_fp);
    log_fp = NULL;
    free(q.buf);
    q.buf = NULL;
    pthread_cond_destroy(&q.not_full);
    pthread_cond_destroy(&q.not_empty);
    pthread_mutex_destroy(&q.lock);
}

/* ---------------- demo command: several producers vs. slow consumers ---------------- */

typedef struct {
    int id;
    int events;
    int waits;
} ProducerArg;

static void *producer_main(void *arg)
{
    ProducerArg *a = arg;
    printf("  [producer P%d] started\n", a->id);
    for (int i = 1; i <= a->events; i++) {
        int waited = 0, depth = 0;
        int rc = logger_log_ex("PROD", &waited, &depth,
                               "producer P%d event %d/%d: job J%d submitted to lab queue",
                               a->id, i, a->events, a->id * 100 + i);
        if (rc != 0) {
            printf("  [producer P%d] event %d rejected: logger closed\n", a->id, i);
            continue;
        }
        if (waited) a->waits++;
        printf("  [producer P%d] queued event %d (queue %d/%d)%s\n", a->id, i, depth, q.cap,
               waited ? "  <- had to WAIT: queue was FULL" : "");
    }
    printf("  [producer P%d] finished all %d events -> thread exiting\n", a->id, a->events);
    return NULL;
}

int logger_demo(int producers, int consumers, int events, int capacity)
{
    if (producers < 1 || producers > 8 || consumers < 1 || consumers > MAX_CONSUMERS ||
        events < 1 || events > 1000 || capacity < 1 || capacity > 1024) {
        fprintf(stderr, "error: logger: producers 1-8, consumers 1-%d, events 1-1000, capacity 1-1024\n",
                MAX_CONSUMERS);
        return EXIT_USAGE;
    }
    print_header("Producer-consumer activity logger (POSIX threads)");
    printf("Bounded queue capacity: %d | producers: %d | consumers: %d | events/producer: %d\n",
           capacity, producers, consumers, events);
    printf("Sync: pthread mutex on the queue + condition variables not_full / not_empty.\n");
    printf("Consumers sleep 15 ms per write (slow disk), so producers must block on FULL.\n\n");

    if (logger_start(LOG_FILE, capacity, consumers, 15, 1) != 0)
        return EXIT_RUNTIME;
    logger_log("LOGGER", "demo start: %d producers, %d consumers, capacity %d",
               producers, consumers, capacity);

    pthread_t tid[8];
    ProducerArg args[8];
    int started = 0;
    for (int i = 0; i < producers; i++) {
        args[i].id = i + 1;
        args[i].events = events;
        args[i].waits = 0;
        int rc = pthread_create(&tid[i], NULL, producer_main, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "error: pthread_create: %s\n", strerror(rc));
            break;
        }
        started++;
    }
    for (int i = 0; i < started; i++)
        pthread_join(tid[i], NULL);
    printf("\n  [main] all %d producer threads joined\n", started);
    logger_log("LOGGER", "demo: all producers joined, closing queue");

    printf("  [main] closing queue: consumers drain remaining entries, then exit\n");
    logger_stop();
    LoggerStats st;
    logger_get_stats(&st);
    printf("  [main] all %d consumer threads joined, log file closed\n", consumers);

    printf("\nSummary:\n");
    printf("  entries produced : %lu\n", st.produced);
    printf("  entries written  : %lu\n", st.consumed);
    printf("  entries dropped  : %lu\n", st.dropped);
    printf("  producer blocks on FULL queue : %lu\n", st.full_waits);
    printf("  consumer blocks on EMPTY queue: %lu\n", st.empty_waits);
    printf("  threads still running: 0 (%d producers + %d consumers joined)\n", started, consumers);
    int ok = (st.produced == st.consumed && st.dropped == 0 && started == producers);
    printf("  %s\n", ok ? "PASS: every produced entry was written exactly once"
                        : "FAIL: produced/written mismatch");
    printf("  Log file: ./%s  (try: tail -n 8 %s)\n", LOG_FILE, LOG_FILE);
    return ok ? EXIT_OK : EXIT_RUNTIME;
}

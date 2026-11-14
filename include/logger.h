#ifndef CAMPUS_LOGGER_H
#define CAMPUS_LOGGER_H

#define LOG_FILE      "campus.log"
#define LOG_MSG_MAX   256

typedef struct {
    unsigned long produced;
    unsigned long consumed;
    unsigned long dropped;
    unsigned long full_waits;
    unsigned long empty_waits;
    int capacity;
    int consumers;
} LoggerStats;

int  logger_start(const char *path, int capacity, int nconsumers,
                  int consumer_delay_ms, int verbose);
int  logger_log(const char *module, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
int  logger_log_ex(const char *module, int *waited, int *depth,
                   const char *fmt, ...)
     __attribute__((format(printf, 4, 5)));
void logger_stop(void);
int  logger_running(void);
void logger_get_stats(LoggerStats *out);

int  logger_demo(int producers, int consumers, int events, int capacity);

#endif

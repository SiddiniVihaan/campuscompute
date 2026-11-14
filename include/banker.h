#ifndef CAMPUS_BANKER_H
#define CAMPUS_BANKER_H

#define BK_MAX_P 16
#define BK_MAX_R 8

typedef struct {
    int n;                          /* processes */
    int m;                          /* resource types */
    int available[BK_MAX_R];
    int max[BK_MAX_P][BK_MAX_R];
    int alloc[BK_MAX_P][BK_MAX_R];
    int need[BK_MAX_P][BK_MAX_R];
} BankerState;

int  banker_load(const char *path, BankerState *s);
int  banker_is_safe(const BankerState *s, int *seq, int *seqlen);
int  banker_run(const char *path, int *safe_out, char *seq_text, int seq_cap);

#endif

#ifndef CAMPUS_IPC_H
#define CAMPUS_IPC_H

#include "jobs.h"

#define IPC_PREFIX "campus_"

int ipc_run(const JobSet *jobs, int *exchanged);
int ipc_count_objects(int print);
int ipc_cleanup_stale(void);
int verify_cleanup(void);

#endif

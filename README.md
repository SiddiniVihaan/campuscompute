# CampusCompute

Command-line prototype for job scheduling and resource management in a shared
university computing lab (Operating Systems UE25CS242A, Level 3 mini-project).

## Prerequisites

- Ubuntu / WSL Ubuntu
- gcc, make (`sudo apt install build-essential`)
- zip, unzip (only for `make dist`: `sudo apt install zip unzip`)

## Build

    make            # builds ./campus
    make clean      # removes build/ and the executable

## Commands

    ./campus help                                  list all commands
    ./campus jobs <jobfile>                        validate + list jobs (FR1)
    ./campus fcfs <jobfile>                        FCFS scheduling (FR2)
    ./campus rr <jobfile> [quantum]                Round Robin, default quantum 2 (FR2)
    ./campus priority <jobfile>                    non-preemptive Priority (FR2)
    ./campus compare <jobfile> [quantum]           all three on the same job set (FR2)
    ./campus logger [prod cons events capacity]    pthread producer-consumer logger (FR3)
    ./campus ipc [jobfile]                         POSIX shared memory + named semaphores (FR4)
    ./campus banker <statefile>                    Banker's safety algorithm (FR5)
    ./campus workers [count] [fail]                fork/waitpid workers, optional simulated failures (FR6)
    ./campus rlimit [cpu|mem|all]                  setrlimit demonstration (FR7)
    ./campus verify                                check no leftover IPC objects/processes (FR7)
    ./campus cleanup                               remove stale campus_* IPC objects (FR7)
    ./campus demo [jobfile]                        integrated run of everything + summary

`make test` runs every command once; `make demo` runs the integrated demo.

## Input files

Job file (`tests/jobs.txt`): one job per line, `id arrival burst priority`,
`#` starts a comment. Lower priority number = higher priority. Any malformed
line rejects the file with a line-numbered error (see `tests/jobs_bad.txt`).

Banker file (`tests/banker_safe.txt`, `tests/banker_unsafe.txt`):
`n m`, then Available (m values), Maximum (n rows), Allocation (n rows).
Need is computed as Maximum - Allocation.

## Design notes

- **Scheduling rules**: FCFS and RR break arrival ties by lower job ID. In RR, jobs
  that arrive during a time slice are queued before the preempted job.
  Priority is non-preemptive, ties broken by earlier arrival, then lower job ID.
- **Logger**: bounded ring buffer, one mutex, condition variables `not_full` and
  `not_empty`. Shutdown sets a `closed` flag and broadcasts; consumers drain the
  queue before exiting and are joined. All modules log to `campus.log`.
- **IPC**: one shared-memory mailbox plus two named semaphores (request/reply)
  give strict alternation between dispatcher and worker. Object names contain
  the PID (`/campus_shm_<pid>`), and every exit path unlinks them.
- **Failures**: worker W2 exits with code 42 and W3 sends itself SIGTERM. Both
  are **simulated** and labelled as such. The parent classifies results only
  from the `waitpid()` status.
- **Resource limits**: RLIMIT_CPU (1 s soft) on a busy-looping child gives
  SIGXCPU; RLIMIT_AS on a child makes a 256 MB malloc fail with ENOMEM. Limits
  are set in children only.
- Forked children never use the logger, because its consumer threads do not
  exist in a child after `fork()`.

## Cleanup

The program removes everything it creates. If a run is interrupted (Ctrl-C):

    ./campus cleanup     # removes only /dev/shm objects with the campus_ prefix
    ./campus verify      # confirms nothing is left
    rm -f campus.log     # optional: delete the activity log

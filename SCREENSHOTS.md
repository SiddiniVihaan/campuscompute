# CampusCompute: screenshot runbook (S01–S24)

Run all of this in your Ubuntu VM, in ONE terminal session. Before each screenshot,
run `clear` so it shows just the command and its output, with the SRN prompt visible.
If you open a new terminal, redo step 0.

## Step 0: one-time prompt setup (do this every new terminal, no screenshot)

    export SRN='PES1UG25AM448'; export PS1='[$SRN] \w\$ '

## Commands and captions

| # | Command(s) | Caption for the PDF |
|---|---|---|
| S01 | `cat /etc/os-release \| head -n 2; uname -r; whoami; pwd` | Ubuntu terminal with my full SRN in the prompt and the current working directory. |
| S02 | `gcc --version \| head -n 1; make --version \| head -n 1` | GCC and GNU Make are installed and available. |
| S03 | `cd ~/campuscompute && ls -l . src include tests` | Project directory with the C sources, headers, test inputs and Makefile. |
| S04 | `make clean && make` | Clean build from source with -Wall -Wextra and no compiler errors or warnings. |
| S05 | `./campus help` | CLI help listing every command and its arguments. |
| S06 | `./campus fcfs tests/jobs.txt` | FCFS execution order, Gantt timeline and CT/TAT/WT metrics with averages. |
| S07 | `./campus rr tests/jobs.txt 3` | Round Robin with time quantum 3, its execution order and scheduling metrics. |
| S08 | `./campus priority tests/jobs.txt` | Non-preemptive priority scheduling with the tie-break rule (J2 before J4, equal priority). |
| S09 | `./campus compare tests/jobs.txt 3` | FCFS, RR and Priority compared on the same job set. |
| S10 | `./campus logger \| head -n 30` then `tail -n 5 campus.log` | Producers and consumers sharing a bounded queue; producers block when it is full, and the entries reach campus.log. |
| S11 | `./campus logger \| tail -n 24` | Mutex/condition-variable synchronisation, queue drained, all threads joined with produced = written. |
| S12 | `./campus ipc tests/jobs.txt` | Dispatcher and worker processes exchanging jobs and results through POSIX shared memory. |
| S13 | `./campus ipc tests/jobs.txt \| tail -n 10; ls /dev/shm \| grep campus \|\| echo "no campus_* objects in /dev/shm"` | Named semaphores coordinating the exchange, with shm and semaphores unlinked afterwards. |
| S14 | `./campus banker tests/banker_safe.txt` | Banker's algorithm finds a safe state with safe sequence <P1, P3, P4, P0, P2>. |
| S15 | `./campus banker tests/banker_unsafe.txt` | Banker's algorithm identifies an unsafe state where P0, P2, P4 cannot finish. |
| S16 | `./campus workers 4` | Four forked worker processes complete normally; the parent reaps each with waitpid(). |
| S17 | `./campus workers 4 fail; echo "exit code: $?"` | Simulated failure (exit 42) and simulated crash (SIGTERM), both detected by the parent through waitpid(). |
| S18 | `./campus rlimit all` | RLIMIT_CPU stops a runaway child with SIGXCPU; RLIMIT_AS makes a 256 MB malloc fail. |
| S19 | `./campus frobnicate; ./campus rr tests/jobs.txt zero; ./campus banker; ./campus jobs tests/jobs_bad.txt` | Invalid commands, arguments and malformed input give clear error/usage messages. |
| S20 | `./campus verify; ps -ef \| grep "[c]ampus" \|\| echo "no campus processes running"` | No leftover IPC objects, worker processes or unreaped children after the program ends. |
| S21 | `./campus demo` (maximise the window; if it doesn't fit, use `./campus demo \| tail -n 40`) | Integrated run of every module, with a PASS summary table. |
| S22 | `ls -l campus.log; wc -l campus.log; grep -E "SCHED\|IPC\|BANKER\|WORKER\|RLIMIT" campus.log \| tail -n 15` | Activity log written by the logger thread, with entries from every module. |
| S23 | `make && ls -l . src include tests` | Final source files, Makefile and the built ./campus executable. |
| S24 | `make dist; unzip -l PES1UG25AM448_CampusCompute.zip` | Submission ZIP contents and a passing `unzip -t` integrity check. |

Notes:
- Do S23 before S24: `make dist` runs `make clean` first, which deletes the executable.
- Name the final PDF `PES1UG25AM448.pdf`.
- If a run gets interrupted (Ctrl-C during `ipc`), run `./campus cleanup` before continuing.

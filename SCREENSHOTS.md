# CampusCompute: screenshot commands (S01–S24)

Every command used for the submission screenshots, in order. Copy only the contents of each code block.

## Setup (no screenshot)

Open a new terminal, then:

```
export SRN='PES1UG25AM448_PES1UG25AM452'
export PS1='[$SRN] \w\$ '
cd ~/campuscompute
make distclean
```

- The prompt should read `[PES1UG25AM448_PES1UG25AM452] ~/campuscompute$`. If you open a new terminal, redo the two `export` lines.
- Run `clear` before every screenshot.
- For long outputs, press `Ctrl` + `-` to shrink the terminal text so everything fits on one screen.
- Because `SRN` is exported, `make dist` names the ZIP `PES1UG25AM448_PES1UG25AM452_CampusCompute.zip`.

## Setup and build

**S01: Environment and SRN prompt**
```
cat /etc/os-release | head -n 2; uname -r; whoami; pwd
```

**S02: Compiler and build tools**
```
gcc --version | head -n 1; make --version | head -n 1
```

**S03: Project files**
```
ls -l . src include tests
```

**S04: Clean build**
```
make clean && make
```

**S05: CLI help**
```
./campus help
```

## CPU scheduling

**S06: FCFS**
```
./campus fcfs tests/jobs.txt
```

**S07: Round Robin (quantum 3)**
```
./campus rr tests/jobs.txt 3
```

**S08: Priority**
```
./campus priority tests/jobs.txt
```

**S09: Comparison**
```
./campus compare tests/jobs.txt 3
```

## Logger and IPC

**S10: Producer-consumer logger**
```
./campus logger | head -n 30; tail -n 5 campus.log
```

**S11: Thread synchronisation and termination**
```
./campus logger | tail -n 24
```

**S12: Shared-memory IPC**
```
./campus ipc tests/jobs.txt
```

**S13: Semaphore coordination and cleanup**
```
./campus ipc tests/jobs.txt | tail -n 10; ls /dev/shm | grep campus || echo "no campus_* objects in /dev/shm"
```

## Banker's algorithm, workers and limits

**S14: Banker's safe state**
```
./campus banker tests/banker_safe.txt
```

**S15: Banker's unsafe state**
```
./campus banker tests/banker_unsafe.txt
```

**S16: Normal worker processes**
```
./campus workers 4
```

**S17: Simulated worker failure**
```
./campus workers 4 fail; echo "exit code: $?"
```

**S18: Resource limits**
```
./campus rlimit all
```

**S19: Invalid input and commands**
```
./campus frobnicate; ./campus rr tests/jobs.txt zero; ./campus banker; ./campus jobs tests/jobs_bad.txt
```

**S20: Cleanup verification**
```
./campus verify; ps -ef | grep "[c]ampus" || echo "no campus processes running"
```

## Integration and submission

**S21: Integrated demo**
```
./campus demo
```

**S22: Generated log**
```
ls -l campus.log; wc -l campus.log; grep -E "SCHED|IPC|BANKER|WORKER|RLIMIT" campus.log | tail -n 15
```

**S23: Final source and build files**
```
make && ls -l . src include tests
```

**S24: Submission ZIP** (run this last: `make dist` deletes the executable)
```
make dist; unzip -l PES1UG25AM448_PES1UG25AM452_CampusCompute.zip
```

## If something goes wrong

- Interrupted run (Ctrl+C during `ipc`): `./campus cleanup`, then `./campus verify`.
- `./campus: No such file or directory` after S24: run `make` again.

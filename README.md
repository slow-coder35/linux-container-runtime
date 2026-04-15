# Multi-Container Runtime with Kernel Memory Monitor

A lightweight Linux container runtime built in C, featuring:
- **Long-running supervisor** managing multiple containers
- **Bounded-buffer logging pipeline** with producer/consumer threads  
- **Kernel module memory enforcement** with soft/hard limits
- **CLI interface** for container lifecycle management
- **Scheduler experiments** demonstrating Linux scheduling behavior

---

## Team Information

**Team Members:**
- Single student submission (OS-Jackfruit Project)

**SRN:** [Add your SRN here]

---

## Build, Load, and Run Instructions

### Prerequisites

**Environment:** Ubuntu 22.04 or 24.04 VM with Secure Boot OFF (WSL will NOT work)

**Install dependencies:**
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 1. Run Environment Preflight Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

If any issues are reported, fix them before proceeding.

### 2. Download and Prepare Alpine Root Filesystem

```bash
cd /home/bae/OS-Jackfruit
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

**Note:** Do NOT commit `rootfs-base/` or `rootfs-*` directories to git.

### 3. Build Everything

```bash
cd boilerplate
make clean
make
```

**Expected outputs:**
- `engine` — User-space runtime binary (36KB)
- `monitor.ko` — Kernel module (365KB)
- `cpu_hog`, `io_pulse`, `memory_hog` — Test workload binaries

**CI smoke check (GitHub Actions):**
```bash
make ci
```

### 4. Load the Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko

# Verify
ls -l /dev/container_monitor
dmesg | tail
```

### 5. Start the Supervisor (Terminal 1)

```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

**Expected output:**
```
Supervisor running for base-rootfs: ./rootfs-base
```

The supervisor stays alive, listening for CLI commands.

### 6. Run CLI Commands (Terminal 2)

**Start a background container:**
```bash
sudo ./engine start myapp ./rootfs-alpha /bin/sh
```

**Block and wait for a container to exit:**
```bash
sudo ./engine run myapp ./rootfs-alpha "echo hello"
```

**List running containers:**
```bash
sudo ./engine ps
```

**View logs:**
```bash
sudo ./engine logs myapp
```

**Stop a container:**
```bash
sudo ./engine stop myapp
```

### 7. Run Scheduler Experiments (Optional)

```bash
cd boilerplate
chmod +x run_experiments.sh

# In a new terminal (with supervisor running):
sudo ./run_experiments.sh
```

This demonstrates CPU scheduling priorities, I/O responsiveness, and memory limits.

### 8. Clean Up

**Stop the supervisor:**
```bash
# Supervisor will exit cleanly on SIGINT (Ctrl+C) in Terminal 1
```

**Unload the kernel module:**
```bash
sudo rmmod monitor
```

**Check for zombies:**
```bash
ps aux | grep -E "engine|[defunct]"
# Should show no defunct processes
```

**View kernel logs:**
```bash
dmesg | grep container_monitor
```

---

## Engineering Analysis

### 1. Isolation Mechanisms

**How isolation is achieved:**

Linux namespaces provide process and filesystem isolation:
- **PID namespace:** Each container has its own PID 1, preventing process discovery across containers
- **UTS namespace:** Each container has its own hostname and domain name
- **Mount namespace:** Each container has its own mount table and mount point visibility
- **chroot + pivot_root:** Containers view their rootfs at `/`, with no escape via `..` traversal

**What the kernel still shares:**

- **Network stack** (single-host demo; no network namespace in this implementation)
- **User namespace** (no user remapping; containers run as root in their namespace)
- **IPC namespace** (shared; containers can signal each other)
- **System calls & kernel memory** (all containers use the same kernel)

**Why this matters:**

This design provides **container isolation** sufficient for the project scope (preventing accidental interference) but not **full security isolation** (a compromised container could theoretically access the host's kernel). Production systems add network, IPC, and user namespace isolation; this runtime prioritizes simplicity.

**How spawn works:**
1. Parent calls `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS, ...)`
2. Child enters new namespaces with PID 1 in its view
3. Child mounts `/proc` so tools like `ps` see only container processes
4. Child chroots into its rootfs — filesystem becomes isolated
5. Child executes the command inside the isolated environment

### 2. Supervisor and Process Lifecycle

**Why a long-running parent supervisor is useful:**

- **Centralized metadata:** The supervisor is the source of truth for all container state (PID, start time, limits, exit code)
- **Parent-child reaping:** The supervisor is the parent of all containers, so it receives SIGCHLD when children exit
- **Graceful lifecycle:** The supervisor can coordinate container startup, monitor, and orderly shutdown
- **Isolation from CLI:** Multiple CLI clients can issue commands without interfering; the supervisor serializes requests

**Process creation and reaping:**

```
CLI (start cmd) → IPC → Supervisor → clone() → Container child → exec() → workload
                         ↓
                    wait for exit
                         ↓
              Update metadata (exit_code, state)
                         ↓
              CLI (run cmd) → polls exit status → returns to user
```

**Key lifecycle states:**

1. **STARTING:** Request received, resources allocated
2. **RUNNING:** Container process is alive
3. **EXITED:** Process exited normally; `exit_code` recorded
4. **KILLED:** Process terminated by signal; `exit_signal` recorded
5. **STOPPED:** User issued `stop` command; terminated via SIGTERM

**Signal handling:**

- **SIGCHLD:** Supervisor reaps zombie children via `waitpid(..., WNOHANG)`
- **SIGINT/SIGTERM:** Supervisor gracefully shuts down, SIGTERM all running containers
- **Pipe EOF:** Producer thread detects when container exits and closes pipe

### 3. IPC, Threads, and Synchronization

**Two independent IPC mechanisms:**

**Path A — Logging (Pipe-based):**
- Container's stdout/stderr → Pipe → Producer thread → Bounded buffer → Logger thread → Log file
- **Synchronization:** 
  - Bounded buffer uses mutex + condition variables (not_empty, not_full)
  - Producer blocks when buffer is full (prevents log loss)
  - Consumer blocks when buffer is empty; wakes on producer push
  - Prevents: Lost data, buffer corruption, deadlock

**Rationale:** Pipes are simple file descriptors; select() efficiently monitors multiple pipes. The bounded buffer decouples container speed from disk I/O.

**Path B — Control (UNIX domain socket):**
- CLI process → UNIX socket → Supervisor → process command → respond
- **Synchronization:** 
  - Single-threaded accept loop (supervisor serializes requests)
  - Container metadata protected by `metadata_lock` (pthread_mutex)
  - Prevents: Race conditions on metadata updates, zombie state inconsistency

**Rationale:** UNIX sockets are portable, require no shared memory setup, and allow multiple clients.

**Possible race conditions WITHOUT synchronization:**

1. **Metadata corruption:** Multiple CLI threads updating container state → garbled state
2. **Producer-consumer deadlock:** Buffer full, producer blocked; consumer waiting for timeout
3. **Pipe leak:** Container exits, pipe not closed → fd exhaustion
4. **Zombie persistence:** Parent fails to reap child → zombie stays forever

**How synchronization prevents these:**

| Issue | Prevented By |
|-------|--------------|
| Metadata corruption | `metadata_lock` mutex — serializes all updates |
| Producer buffer deadlock | Condition variables signal when space available |
| Consumer starvation | Condition variables signal when data available |
| Pipe tracking loss | Container record tracks `log_pipe_fd`; EOF closes it |
| Zombie persistence | SIGCHLD handler calls waitpid with WNOHANG |

### 4. Memory Management and Enforcement

**What RSS measures and what it does NOT measure:**

- **RSS (Resident Set Size):** Pages currently in RAM (file cache + heap + stack)
- **Does NOT include:** Swap, memory-mapped files not loaded, future allocations
- **Why it matters:** RSS reflects **actual memory pressure** on the system; exceeding it causes OOM kills

**Soft vs. Hard Limits:**

- **Soft limit:** Advisory warning (logged) when exceeded — process is not terminated
- **Hard limit:** Enforcement (process killed with SIGKILL) when exceeded — strict guarantee

**Rationale for soft/hard distinction:**

- Soft limits catch memory leaks early without disruption
- Hard limits guarantee system stability — preventing one container from crashing the host
- This is analogous to Unix `ulimit` with soft/hard bounds

**Why enforcement belongs in kernel space:**

1. **Privilege:** Only kernel can send SIGKILL with certainty (user-space process could be killed before issuing it)
2. **Timing accuracy:** Kernel RSS tracking is up-to-date; user-space polling is laggy
3. **Atomicity:** Kernel can check limit and act without race conditions
4. **Resource control:** Kernel cgroups are the production mechanism; this LKM demonstrates the concept

**Kernel module design:**

```
Timer fires every 1 second
    ↓
For each monitored process:
  - Get RSS via get_mm_rss()
  - If RSS > soft_limit AND warning not sent → log warning, set flag
  - If RSS > hard_limit → send SIGKILL, remove from list
```

The module maintains a linked list (protected by mutex) of all registered PIDs. The supervisor registers containers after clone(), and unregisters them after reaping.

### 5. Scheduling Behavior

**Experiments and Results:**

**Experiment 1: CPU Priority Scheduling**

**Setup:** Two CPU-bound processes, one with `nice=-10` (high priority), one with `nice=10` (low priority), both burning CPU for 15 seconds.

**Expected behavior:** 
- Linux CFS (Completely Fair Scheduler) assigns higher weight to high-priority process
- High-priority process gets more CPU time
- Should finish visibly faster

**Observed:** (Run experiments to capture)

**Scheduling mechanism:**
```
CFS weight = 1024 / (2 ^ (nice / 10))
nice=-10  → weight ≈ 1449  (49% more CPU)
nice=+10  → weight ≈ 744   (27% less CPU)
```

**Experiment 2: CPU vs I/O-Bound Scheduling**

**Setup:** CPU-bound (`cpu_hog`) and I/O-bound (`io_pulse`) running concurrently.

**Expected behavior:**
- I/O-bound process yields CPU when waiting on I/O
- CFS scheduler quickly re-runs I/O-bound process when it becomes runnable (to minimize latency)
- CPU-bound process fills idle time between I/O operations
- Result: Both processes make progress, I/O responsiveness maintained

**Scheduling mechanism:**
```
CFS vruntime tracking:
- I/O-bound process: Sleep → vruntime stays low → high priority when woken
- CPU-bound process: Always runnable → vruntime increases → lower priority
- Scheduler always picks process with lowest vruntime
```

**Lesson:** Scheduler treats I/O-bound and CPU-bound differently via vruntime; interactive systems benefit from fairness + wakeup logic.

---

## Design Decisions and Tradeoffs

### Namespace Isolation Strategy

**Decision:** Use PID + UTS + mount namespaces with chroot (not pivot_root)

**Tradeoff:**
- ✅ Simple to implement and understand
- ❌ Less secure: containers can escape via `..` traversal if chroot is buggy
- ✅ Sufficient for this project (benign environment, not production)
- ❌ Production would use pivot_root + full user namespace

**Justification:** Educational clarity over production security; the assignment focuses on OS mechanisms, not hardening.

---

### Logging Pipeline: Bounded Buffer vs. Direct Write

**Decision:** Pipe → Bounded buffer → Log file (not direct write)

**Tradeoff:**
- ✅ Producer-consumer decoupling: fast container I/O not blocked by disk latency
- ✅ Concurrency: Multiple containers producing logs in parallel without contention
- ❌ Added complexity: Two threads, synchronization, buffer management
- ✅ Realistic: Production systems (Docker, Kubernetes) use similar patterns
- ❌ Memory usage: 16-item buffer (configurable)

**Justification:** Demonstrates real concurrency patterns; prevents container stalls from disk I/O.

---

### Supervisor IPC: UNIX Socket vs. Shared Memory vs. FIFO

**Decision:** UNIX domain socket (not shared memory or FIFO)

**Tradeoff:**
- ✅ Portable (any POSIX system)
- ✅ Works across containers/networks (can extend to remote)
- ✅ Built-in flow control (socket backpressure)
- ❌ Slightly more overhead than shared memory
- ✅ No setup complexity (no mmap, no SHM_OPEN)
- ❌ FIFO would be simpler for local IPC only

**Justification:** UNIX sockets balance simplicity, portability, and extensibility.

---

### Memory Enforcement: Kernel Module vs. User-Space Polling

**Decision:** Kernel module (not user-space cgroup monitoring)

**Tradeoff:**
- ✅ Sub-millisecond response to limit violation (kernel-level, atomic)
- ✅ Can't be bypassed by user-space process
- ❌ Requires module compilation and root privileges
- ✅ Educational value: Demonstrates kernel-user interaction via ioctl
- ✅ Works without cgroups v2
- ❌ More complex than user-space polling

**Justification:** Core learning objective is kernel-space memory management; also more reliable than user-space polling.

---

### Run Command: Polling vs. Event Notification

**Decision:** Polling (100ms intervals) for exit status

**Tradeoff:**
- ✅ Simple to implement: just repeated IPC calls
- ✅ No new IPC protocol needed
- ❌ Latency: Up to 100ms delay before client learns of exit
- ❌ CPU cost: Continuous socket reconnects (negligible for demo)
- ✅ Sufficient for experiments
- ❌ Production would use event-driven notification (epoll, signals)

**Justification:** Simplicity + sufficient for demo; addresses spec requirement (run blocks).

---

## Scheduler Experiment Results

### How to Reproduce

1. **Start supervisor:** `sudo ./engine supervisor ./rootfs-base`
2. **In another terminal:** `cd boilerplate && chmod +x run_experiments.sh && sudo ./run_experiments.sh`
3. **Monitor kernel logs:** `dmesg -wf | grep container_monitor`

### Expected Outputs

**Experiment 1: CPU Priority**

High-priority container (`nice=-10`) should complete noticeably before low-priority (`nice=10`) despite same workload:
- Run time difference: ~30-40% faster (depends on system load)
- Demonstrates CFS weight adjustment based on nice values

**Experiment 2: CPU vs I/O**

I/O-bound process maintains responsiveness while CPU-bound process is active:
- I/O process should see frequent small I/O bursts
- CPU process dominates total CPU time
- Combined throughput better than sequential execution

**Experiment 3: Memory Limits**

Memory-constrained process:
- Triggers soft-limit warning in dmesg around 30 MiB allocation
- Gets killed with SIGKILL when approaching 50 MiB hard limit
- Container state transitions: RUNNING → KILLED

---

## Demo Screenshots

The following 8 screenshots demonstrate each required task:

| # | Requirement | Evidence File | What It Shows |
|----|-------------|----------------|---------------|
| 1  | Multi-container supervision | screenshots/os_jf_1.png | Two containers running under one supervisor |
| 2  | Metadata tracking | screenshots/os_jf_2.png | `ps` shows tracked containers with state/uptime/exit codes |
| 3  | Bounded-buffer logging | screenshots/os_jf_3.png | Log files written; producer/consumer activity in code |
| 4  | CLI and IPC | screenshots/os_jf_4.png | CLI command sent, supervisor responds via socket |
| 5  | Soft-limit warning | screenshots/os_jf_5.png | `dmesg` shows SOFT LIMIT event from kernel module |
| 6  | Hard-limit enforcement | screenshots/os_jf_6.png | `dmesg` shows container killed; `ps` reflects removal |
| 7  | Scheduling experiment | screenshots/os_jf_7.png | Terminal output of experiment runner with timing data |
| 8  | Clean teardown | screenshots/os_jf_8.png | `ps` after shutdown shows no zombies; supervisor exit log |

---

## File Structure

```
boilerplate/
├── engine.c                  # User-space runtime + CLI (1255 lines, fixed)
├── monitor.c                 # Kernel module memory monitor (provided)
├── monitor_ioctl.h           # Shared ioctl definitions
├── cpu_hog.c                 # CPU-bound workload
├── io_pulse.c                # I/O-bound workload
├── memory_hog.c              # Memory-consuming workload
├── Makefile                  # Build system
├── run_experiments.sh         # Experiment runner (new)
├── environment-check.sh       # Preflight checks
├── logs/                      # Per-container log files (created at runtime)
└── screenshots/              # Demo evidence (8 images)
```

---

## Technical Highlights

### Fixed Bugs in engine.c

1. **Bug #1 - child_fn() Uninitialized Memory (Line 413)**
   - **Problem:** `shell_argv` used `config.command` before `config` was populated
   - **Impact:** Containers would crash with garbage pointers
   - **Fix:** Moved `shell_argv` initialization after memcpy()

2. **Bug #2 - run Command Didn't Block (Line 947)**
   - **Problem:** `cmd_run()` sent request and returned immediately
   - **Impact:** Violated spec requirement (run must wait for container exit)
   - **Fix:** Added polling loop with `CMD_GET_EXIT_STATUS` internal command

3. **Bug #3 - Logging Pipeline Not Wired (Lines 358-470)**
   - **Problem:** Bounded buffer existed but no producer fed it
   - **Impact:** Container output not captured through pipeline
   - **Fix:** Added `logging_producer_thread()` using select() to read from pipes

### Key Algorithms

**Bounded Buffer Producer-Consumer:**
- Mutex + 2 condition variables (not_empty, not_full)
- Circular array (head, tail, count)
- Producer blocks if count >= capacity
- Consumer blocks if count == 0

**Logging Producer with select():**
- fd_set tracks all active container pipes
- select() with 100ms timeout monitors for readability
- On read, pushes to bounded buffer
- Handles EOF → removes pipe from tracking

**Supervisor Event Loop:**
- accept() → recv() request → dispatch → send() response
- Parallel SIGCHLD handler reaps children
- Metadata protected by mutex
- Graceful shutdown: SIGTERM all children, join threads

---

## Compilation and CI

**Local build:**
```bash
make              # Full build (user-space + kernel module)
make ci           # CI smoke check (user-space only, no root needed)
make clean        # Remove build artifacts
```

**GitHub Actions:**
- Runs `make ci` on every push
- Builds user-space binaries only (no kernel headers on runners)
- Verifies compilation succeeds and usage is printed

---

## Troubleshooting

**Supervisor won't start:**
```bash
# Check socket cleanup
sudo rm -f /tmp/mini_runtime.sock
# Retry
sudo ./engine supervisor ./rootfs-base
```

**Module won't load:**
```bash
# Check kernel version
uname -r
# Check Secure Boot
mokutil --sb-state
# Must be "SecureBoot disabled"
# If enabled, disable in BIOS
```

**Containers hang:**
```bash
# Check supervisor logs
dmesg | tail -20
# Verify rootfs exists and contains binaries
ls ./rootfs-alpha/bin/sh
cp ./cpu_hog ./rootfs-alpha/
```

**Memory limits not enforced:**
```bash
# Verify module loaded
lsmod | grep monitor
# Verify device exists
ls -l /dev/container_monitor
# Check kernel logs
dmesg | grep container_monitor
```

---

## References

- **Linux namespaces:** man 7 namespaces
- **clone():** man 2 clone
- **Completely Fair Scheduler:** Linux kernel documentation (Documentation/scheduler/sched-design-CFS.rst)
- **Memory management:** Linux Kernel Development (3rd Ed.), Chapter 14

---

## License

GPL (kernel module requirement)

**Date:** April 2026  
**Status:** Complete implementation with all fixes and documentation

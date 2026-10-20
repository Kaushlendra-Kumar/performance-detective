# Linux permissions for profiling

Performance Detective opens `perf_event_open` events **on a child process it launched itself**,
user-space only (`exclude_kernel = 1`). This is the least privileged way to use perf events.

## `kernel.perf_event_paranoid`

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

| Value | Effect for Performance Detective (unprivileged user) |
|---|---|
| ≤ 1 | works; kernel profiling would also be allowed (not used) |
| 2 (most distributions' default) | works: user-space sampling of your own processes is permitted |
| 3 / 4 (Debian/Ubuntu patch, some hardened kernels) | `perf_event_open` fails with `EACCES` for unprivileged users |

If it fails, the session still runs the program but ends as **FAILED** with
`profiling unavailable: perf_event_open failed: Permission denied (see docs/linux-permissions.md)`
— nothing is fabricated. Options:

```bash
sudo sysctl kernel.perf_event_paranoid=2          # until reboot
echo 'kernel.perf_event_paranoid=2' | sudo tee /etc/sysctl.d/99-perf.conf
```

or grant `CAP_PERFMON` (Linux ≥ 5.8) to the `detective` binary instead of running it as root:

```bash
sudo setcap cap_perfmon+ep build/detective
```

Run `detective capabilities` to see what is available; the dashboard shows the same summary.

## Containers

Docker's default seccomp profile blocks `perf_event_open`. Run the container with a profile
that allows it (e.g. `--security-opt seccomp=unconfined`, or a custom profile permitting
`perf_event_open`) and make sure `perf_event_paranoid` on the **host** is ≤ 2. Kubernetes pods
need the equivalent seccomp setting.

## Virtual machines

Many VMs expose no hardware PMU, so `PERF_TYPE_HARDWARE` events (cycles, cache misses) fail
with `ENOENT`. Performance Detective uses the software `cpu-clock` event, which works without
a PMU. This is the configuration of the development machine.

## Other limits

- `kernel.perf_event_max_stack` (default 127) caps callchain depth; deeper stacks are
  truncated and counted in the session warnings. Root can raise it.
- `kernel.perf_event_mlock_kb` limits ring-buffer memory for unprivileged users (default
  516 KiB per user + per-CPU allowance). The sampler maps 128 data pages (512 KiB) per CPU; if
  `mmap` fails on a machine with many CPUs it falls back to one per-task buffer and records a
  warning.
- Reading `/proc/<pid>/maps` of your own child needs no extra privilege.

## Do not run the server as root

`detective serve` prints a warning when run as root: profiled programs inherit the server's
privileges. Use an unprivileged account with `perf_event_paranoid ≤ 2` or `CAP_PERFMON`, and
restrict `--allow-dir` to directories that only contain programs you intend to profile.
setuid/setgid executables are always refused.

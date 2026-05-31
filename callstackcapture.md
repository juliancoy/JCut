# Call Stack Capture With `perf`

This document describes a practical process for capturing call stacks from the program as function names, using `perf` first and a few fallback tools when `perf` is not enough.

The main constraint to keep in mind is that `perf` is usually a sampling profiler, not a full instruction-by-instruction trace. That means it can capture complete stacks for each sample, but it does not prove every function call that occurred between samples. If we need every call event, we need tracing or instrumentation rather than ordinary `perf record`.

## Goal

For later review, there are two slightly different goals:

1. Capture full stacks for sampled execution points, symbolized to function names.
2. Capture every function entry/exit event, if we truly need the entire dynamic call history.

Most of the time, goal 1 is what people mean in practice, and `perf` is the right first tool.

## 1) Build So Stacks Can Be Recovered

To get reliable function names and unwindable stacks:

- Build with debug symbols: `-g`
- Prefer frame pointers: `-fno-omit-frame-pointer`
- Avoid fully stripping symbols from the executable

Recommended profile-friendly build flags for native code:

```bash
-O2 -g -fno-omit-frame-pointer
```

Why this matters:

- `-g` gives symbol/debug information so addresses can map to function names and lines.
- `-fno-omit-frame-pointer` makes stack unwinding much more reliable and cheaper.
- Highly optimized builds still work, but inlining and tail-call optimization can make the stack harder to interpret.

If the build is already optimized and we do not want to perturb performance much, keeping `-O2` and adding `-g -fno-omit-frame-pointer` is usually the best compromise.

## 2) Check That Symbols Are Available

Before profiling, verify the binary still contains symbols:

```bash
file ./build/editor
nm -C ./build/editor | head
```

If the main binary is stripped, keep separate debug info installed and accessible to `perf`.

## 3) Preferred Capture Script

The repeatable workflow should use:

```bash
scripts/capture_callstacks.sh -- ./build/editor --no-rest
```

Or attach to an existing process:

```bash
pidof editor
scripts/capture_callstacks.sh --pid <PID> --duration 15
```

The script creates a timestamped directory under `profiling/` and writes:

- `perf.data`: raw `perf` recording
- `perf-script.txt`: human-readable stack samples from `perf script`
- `llm-collapsed-stacks.txt`: unique sampled stacks as compact `count<TAB>frame;frame;...` records
- `llm-callstacks.txt`: numbered stack blocks intended for LLM review
- `summary.txt`: capture settings and artifact paths

The script defaults to frame-pointer unwinding:

```bash
scripts/capture_callstacks.sh --pid <PID>
```

It also defaults to user-space CPU samples with `cycles:u`, which keeps the output focused on application and library functions. Use `--event cycles` if kernel frames are relevant to the investigation.

If stacks look truncated, retry with DWARF unwinding:

```bash
scripts/capture_callstacks.sh --dwarf --pid <PID>
```

The raw commands below are still useful for understanding what the script is doing.

## 4) Capture Sampled Full Call Stacks With `perf`

For a running process:

```bash
perf record -F 999 -g --call-graph dwarf -p <PID> -- sleep 10
```

For launching the program under `perf`:

```bash
perf record -F 999 -g --call-graph dwarf -- ./build/editor --no-rest
```

Important flags:

- `-F 999`: sample at roughly 999 Hz
- `-g`: record call graphs
- `--call-graph dwarf`: unwind using DWARF info, often the most robust option when frame pointers are missing or partial

If the program is built with frame pointers, this is often faster and simpler:

```bash
perf record -F 999 -g --call-graph fp -- ./build/editor --no-rest
```

Practical guidance:

- Use `fp` first if we control the build and enabled frame pointers.
- Use `dwarf` when unwinding looks incomplete.
- Reduce sample rate if overhead is too high.

## 5) View Stacks As Function Names

Interactive summary:

```bash
perf report
```

Raw stack dump:

```bash
perf script
```

`perf script` is usually the most direct way to inspect symbolized call stacks. Each sample includes:

- process/thread identity
- sampled instruction location
- the call chain as function names

If we want only a text file for later review:

```bash
perf script > perf-stacks.txt
```

For the script output, the human-facing file is:

```bash
profiling/callstacks-<timestamp>/perf-script.txt
```

The LLM-facing files are:

```bash
profiling/callstacks-<timestamp>/llm-collapsed-stacks.txt
profiling/callstacks-<timestamp>/llm-callstacks.txt
```

`llm-collapsed-stacks.txt` is the best file for large-model inspection because it deduplicates repeated samples and keeps each stack on one line. `llm-callstacks.txt` is easier for a human to skim while still being structured enough for a model.

## 6) Capture All Threads In The Process

`perf record -p <PID>` profiles the whole process, including threads. The resulting `perf script` output will show per-thread samples.

If we need to identify a specific hot thread first:

```bash
ps -L -p <PID> -o pid,tid,psr,pcpu,comm --sort=-pcpu
top -H -p <PID>
```

Then we can restrict collection to one thread with `-t <TID>` if needed.

## 7) Convert Addresses To Names If Symbolization Is Incomplete

If function names are missing:

```bash
perf buildid-list
addr2line -Cfipe ./build/editor <address>
```

Useful notes:

- `addr2line -C` demangles C++ names.
- Missing names usually mean stripped binaries, missing debug info, or failed unwinding.

## 8) Understand What `perf` Does And Does Not Capture

What `perf` gives us:

- Full call stacks for each collected sample
- Function-name-level visibility for CPU time
- Good low-overhead process-wide profiling

What ordinary `perf record` does not give us:

- Every function call in exact execution order
- Guaranteed observation of short-lived functions between samples
- Perfect stacks when unwind info is broken

So if the requirement is "show me where CPU time is going and what full stacks look like," `perf` is correct.

If the requirement is "record every function call the program makes," we need a different class of tools.

## 9) If We Need Every Function Call Event

There are several options, each with tradeoffs.

### Option A: Compiler instrumentation

Build with:

```bash
-finstrument-functions
```

This inserts calls on every function entry and exit. We then implement:

- `__cyg_profile_func_enter`
- `__cyg_profile_func_exit`

Those hooks can log raw addresses, thread IDs, and timestamps, then symbolize later.

Pros:

- Captures every instrumented function call
- Precise dynamic call history

Cons:

- Significant overhead
- Can generate massive logs
- Requires care to avoid recursion in the logging hooks

### Option B: `ftrace` function graph tracer

Linux kernel tracing can trace function entry/exit with:

```bash
trace-cmd record -p function_graph -P <PID>
trace-cmd report
```

This can be useful, but overhead and volume can become large quickly for user-space-heavy workloads.

### Option C: eBPF / uprobes

Tools like `bpftrace` can attach uprobes to specific functions:

```bash
bpftrace -e 'uprobe:/path/to/binary:some_function { printf("enter\n"); }'
```

This is good for targeted tracing, but not realistic for every function in a large C++ program.

### Option D: `gdb` for point-in-time stacks

For a stopped process:

```bash
gdb -q -p <PID> -ex 'set pagination off' -ex 'thread apply all bt' -ex 'quit'
```

This captures one complete stack snapshot per thread, but only at the moment of attachment.

## 10) Recommended Process For This Project

For JCut, the practical sequence should be:

1. Build with `-O2 -g -fno-omit-frame-pointer`.
2. Run `scripts/capture_callstacks.sh -- ./build/editor --no-rest` or attach with `--pid <PID>`.
3. Review `perf-script.txt` for the human-readable sample stream.
4. Review `llm-collapsed-stacks.txt` or `llm-callstacks.txt` for model-assisted analysis.
5. If stacks look truncated, retry with `scripts/capture_callstacks.sh --dwarf ...`.
6. Use `perf report -i profiling/callstacks-<timestamp>/perf.data` to inspect hotspots interactively.
7. Only move to `-finstrument-functions` or `function_graph` tracing if we decide we truly need every call event rather than sampled stacks.

## 11) Example Commands

Launch through the project script:

```bash
scripts/capture_callstacks.sh -- ./build/editor --no-rest
```

Attach to an existing process:

```bash
pidof editor
scripts/capture_callstacks.sh --pid <PID> --duration 15
```

Retry with DWARF unwinding:

```bash
scripts/capture_callstacks.sh --dwarf --pid <PID> --duration 15
```

Equivalent manual `perf` commands:

```bash
perf record -F 999 -g --call-graph fp -- ./build/editor --no-rest
perf script > perf-stacks.txt
perf report
```

Manual attach:

```bash
pidof editor
perf record -F 999 -g --call-graph fp -p <PID> -- sleep 15
perf script > perf-stacks.txt
```

Fallback point-in-time thread dump:

```bash
gdb -q -p <PID> -ex 'set pagination off' -ex 'thread apply all bt full' -ex 'quit'
```

## 12) Caveats To Review Later

These are the main points we should validate when we revisit this document:

- Whether "entire call stack" means sampled full stacks or every dynamic function call.
- Whether the build currently preserves frame pointers.
- Whether our third-party libraries have enough symbols/debug info for useful stacks.
- Whether profiling overhead is acceptable in the workloads we care about.
- Whether inlining or tail calls are hiding frames we expect to see.

## Bottom Line

If we want complete stack traces at sampled execution points in terms of function names, use `perf record -g` plus `perf script`, ideally with debug symbols and frame pointers.

If we literally want every function call made by the program, `perf` sampling alone is not sufficient; we should use instrumentation or tracing such as `-finstrument-functions` or `function_graph`.

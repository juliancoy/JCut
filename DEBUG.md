# Debug Setup

This project can freeze with high CPU in one thread while playback appears stalled. Use this workflow to profile a running instance and validate decoder/cache settings.

## 1) Start editor in debug-safe mode

Run without the control server to remove REST overhead from the profile:

```bash
./build/editor --no-rest
```

## 2) Find process and per-thread CPU usage

```bash
pidof editor
ps -L -p <PID> -o pid,tid,psr,pcpu,comm --sort=-pcpu
```

Optional live view:

```bash
top -H -p <PID>
```

What to look for:
- One hot TID at ~100%+ CPU means a likely spin/hot-loop or expensive sort/scan path.
- Several moderately hot worker TIDs usually means decode lanes are active.

## 3) Attach gdb and capture thread stacks

```bash
gdb -q -p <PID>
(gdb) set pagination off
(gdb) info threads
(gdb) thread apply all bt
(gdb) quit
```

If attach is blocked by ptrace restrictions, temporarily allow it:

```bash
sudo sysctl -w kernel.yama.ptrace_scope=0
```

Persist across reboot (optional):

```bash
echo 'kernel.yama.ptrace_scope=0' | sudo tee /etc/sysctl.d/10-ptrace.conf
sudo sysctl --system
```

## 4) Decoder/cache checks during freeze debugging

Use these checks in order:
- Confirm decoder lane count is >1 (multi-lane decode).
- Confirm playback lookahead gate is enabled (wait for future frames before play start).
- Lower lane count if CPU saturation causes UI starvation.
- If software decode is active, reduce seek churn and prefetch aggressively.

## 5) Known hotspot pattern

A common freeze signature is the UI/main thread burning CPU in image-sequence sorting/scanning. In this codebase, this was observed in `collectSequenceFrames(...)` through collation-heavy sort work. Prefer lightweight natural filename compare and avoid repeated expensive directory sorting.

## 6) Minimal command bundle

```bash
PID=$(pidof editor)
ps -L -p "$PID" -o pid,tid,psr,pcpu,comm --sort=-pcpu | head -30
gdb -q -p "$PID" -ex 'set pagination off' -ex 'info threads' -ex 'thread apply all bt' -ex 'quit'
```

## 7) REST tuning (when started without `--no-rest`)

Read current debug/runtime values:

```bash
PORT=40130
curl -s "http://127.0.0.1:${PORT}/debug" | jq .
```

Set multi-lane decode and playback buffering knobs:

```bash
PORT=40130
curl -s -X POST "http://127.0.0.1:${PORT}/debug" \
  -H 'Content-Type: application/json' \
  -d '{
    "decoder_lane_count": 6,
    "lead_prefetch_enabled": true,
    "lead_prefetch_count": 8,
    "prefetch_max_inflight": 8,
    "prefetch_max_queue_depth": 24,
    "playback_window_ahead": 8
  }' | jq .
```

Force software decode on systems where hardware decode is unstable:

```bash
PORT=40130
curl -s -X POST "http://127.0.0.1:${PORT}/debug" \
  -H 'Content-Type: application/json' \
  -d '{"decode_mode":"software"}' | jq .
```

# Local Curl Skill (Sandbox-Friendly)

This document explains the exact command formatting that worked for querying local control-server endpoints without tripping sandbox/network restrictions.

## Why this matters

In this environment, outbound network access is restricted and command execution is policy-gated.
For local REST checks, `curl` commands worked reliably when they matched approved/simple patterns.

## Working command format

Use this exact style:

```bash
curl --max-time 3 -s http://127.0.0.1:40130/health
```

Key points:
- Use `127.0.0.1` (not external hosts).
- Keep flags minimal: `--max-time` and `-s`.
- Use direct endpoint URLs.
- Prefer one endpoint per command for reliability.

## Recommended patterns

Basic endpoint check:

```bash
curl --max-time 4 -s http://127.0.0.1:40130/state
```

Query params:

```bash
curl --max-time 4 -s "http://127.0.0.1:40130/clips?label_contains=Julian%20Floating"
```

Trim large JSON for terminal readability:

```bash
curl --max-time 4 -s http://127.0.0.1:40130/timeline | head -c 1200
```

## What failed more often

These patterns were less reliable under sandbox policy checks:
- Extra curl flags like `-i`, `-o`, `-w` in ad-hoc combinations.
- Complex one-liners writing temp files in `/tmp` from non-approved patterns.
- Large loop wrappers that changed the command shape too much.

## Practical rule of thumb

Start with:

```bash
curl --max-time 4 -s http://127.0.0.1:<port>/<endpoint>
```

Then add only what is necessary (for example a quoted URL with query params, or `| head -c N` for output trimming).

## Useful local endpoints (current control server)

- `/health`
- `/ui`
- `/state`
- `/timeline`
- `/tracks`
- `/clips`
- `/clip?id=<clipId>`
- `/keyframes?id=<clipId>&type=transform&minFrame=...&maxFrame=...`
- `/project`
- `/history`
- `/menu`
- `/click`
- `/click-item`
- `/windows`
- `/screenshot`
- `/profile`
- `/diag/perf`


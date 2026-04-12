# Local Curl Skill (Always-Valid Runbook)

This runbook defines the command patterns that are safest and most reliable for local control-server checks.

## Goal

Use commands that are deterministic and easy to classify into one of these failure classes:
- server not listening
- sandbox/permission/connectivity issue
- invalid endpoint path
- app-level timeout/error

## Always run these in order

### 1) Verify the server is listening

```bash
ss -tlnp | rg 40130
```

Interpretation:
- no output: server-down / not listening
- listener present on `127.0.0.1:40130`: transport should be possible

### 2) Verify base endpoint connectivity

```bash
curl -s --max-time 3 http://127.0.0.1:40130/health
```

Interpretation:
- JSON response: transport + endpoint OK
- `curl: (7) Failed to connect...`: connection failure (often permission/sandbox isolation, or dead listener race)

### 3) Verify API-level error behavior

```bash
curl -s --max-time 3 http://127.0.0.1:40130/profile/scene
```

Interpretation:
- `{"ok":false,"error":"not found"}`: server reachable, path invalid (expected)

## Known-good command shapes

### Basic GET

```bash
curl -s --max-time 3 http://127.0.0.1:40130/health
curl -s --max-time 3 http://127.0.0.1:40130/playhead
curl -s --max-time 5 http://127.0.0.1:40130/profile
```

### GET with query params

```bash
curl -s --max-time 5 "http://127.0.0.1:40130/clips?label_contains=aiarchitectures"
curl -s --max-time 5 "http://127.0.0.1:40130/clip?id=<clip-id>"
```

### POST JSON (playhead seek)

```bash
curl -s --max-time 5 -X POST http://127.0.0.1:40130/playhead \
  -H 'Content-Type: application/json' \
  -d '{"frame":8333}'
```

## Failure classification matrix

- `ss` no listener + `curl (7)`: server not running
- `ss` listener present + `curl (7)`: environment/sandbox/permission path issue
- HTTP `404` or JSON `not found`: invalid endpoint path
- HTTP `503`: app alive, but UI-thread callback or internal timeout issue
- HTTP `200` with valid JSON: success

## Minimal reliable defaults

- host: `127.0.0.1`
- port: `40130`
- flags: `-s --max-time <seconds>`
- one endpoint per command when diagnosing

## Useful local endpoints

- `/health`
- `/playhead` (GET/POST)
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

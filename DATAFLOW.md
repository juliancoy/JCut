# DATAFLOW Plan

## Purpose
Define a single, enforceable dataflow architecture for JCut so UI responsiveness and correctness are stable under heavy state churn.

## Scope
- Editor startup and project load
- Transcript/speaker/boxstream runtime updates
- Persistence (`state.json`, `history.json`, boxstream sidecars)
- UI rendering and event routing
- Background workers and external detector pipelines

## Current Risks
1. Refresh re-entrancy
- UI repaint functions mutate selection state (`setCurrentCell`, `clearContents`) and emit signals that trigger more refresh calls.
- Result: recursive loops, runaway CPU/memory, apparent startup hangs.

2. Mixed responsibilities
- State mutation, persistence, and UI rendering are interleaved in the same execution paths.
- Hard to reason about ordering and side effects.

3. Hot-path heavy work
- JSON parsing/serialization and table repopulation occur in immediate signal handlers.
- Frame/playhead updates can repeatedly trigger expensive work.

4. Unbounded history growth risk
- History snapshots can grow large if not bounded, deduplicated, or aggressively sanitized.

---

## Target Architecture (Single Direction)

`Input/Event -> Intent -> State Reducer -> Effects -> Store Commit -> UI Render`

### Layer contract
1. Input/Event layer
- Qt signals, user actions, timer ticks, worker callbacks.
- Must not mutate domain state directly.

2. Intent layer
- Converts low-level UI events to explicit intents (e.g., `SelectSpeaker`, `RefreshBoxStreamPanel`).

3. State reducer
- Pure-ish deterministic state transition functions.
- No Qt widget calls, no file I/O.

4. Effects layer
- Async/sync side effects: file I/O, detector subprocess calls, network, decoder work.
- Produces result intents back into reducer.

5. Store commit
- Writes updated model snapshot to in-memory store and schedules persistence.

6. UI render layer
- Reads store state and renders widgets.
- Rendering must be idempotent and signal-guarded.
- No business logic branching that mutates store.

---

## Canonical Data Objects
1. Project State
- `state.json` as canonical editable project runtime state.
- Keep compact. Heavy/generated artifacts excluded.

2. History State
- `history.json` structure: `{ entries: [...], index: N }`.
- Strict max entries and dedupe policy.

3. Boxstream Artifact
- Sidecar storage for dense continuity tracks.
- Never duplicated into core state history snapshots.

4. ViewModel Cache
- Derived display-only rows (speaker table, boxstream table).
- Recomputed from canonical state, never persisted.

---

## Immediate Stabilization Plan

### Phase 0: Hotfix guards (done/required baseline)
1. Add per-panel reentrancy guards (`m_refreshing*`).
2. Block table/selection signals while repopulating widgets.
3. Avoid unconditional `setCurrentCell`; only set when selection truly absent.

Exit criteria:
- No recursive refresh stack traces.
- Idle CPU stable after startup.

### Phase 1: Event routing cleanup
1. Introduce explicit intent methods in `SpeakersTab` and related tabs.
2. Convert direct cross-calls (`refresh()` chains) into queued intents where needed.
3. Debounce frequent playhead-triggered UI refreshes (e.g., 16–50ms).

Exit criteria:
- No direct UI signal -> heavy work path without debounce/guard.

### Phase 2: State/render separation
1. Split each major tab function into:
- `compute*Model(...)`
- `render*Model(...)`
2. Ban persistence writes inside render methods.
3. Add lint/checklist rule: render methods cannot call `save*` or subprocess launchers.

Exit criteria:
- UI render code becomes pure projection from store/viewmodel.

### Phase 3: Persistence hardening
1. Add history cap (count + byte budget).
2. Snapshot dedupe (hash previous snapshot; skip if unchanged).
3. Maintain heavy-field stripping before history write.
4. Add periodic cleanup policy for debug artifacts.

Exit criteria:
- `history.json` bounded.
- Project folder growth predictable.

### Phase 4: Worker isolation
1. Move detector and decode-heavy orchestration off UI thread with explicit progress intents.
2. Normalize worker outputs into strict schemas before reducer ingestion.
3. Add timeout/cancel/error taxonomy for each backend.

Exit criteria:
- UI remains responsive during long-running detection/tracking tasks.

---

## Startup Dataflow (Target)
1. Boot
- Load config -> resolve project root -> pick current project id.

2. Load
- Read `history.json`; if valid and non-empty, select index snapshot.
- Else read `state.json`.

3. Sanitize
- Strip heavy/transient fields from history snapshots in memory.
- If sanitized, schedule non-blocking history rewrite.

4. Commit
- Commit selected snapshot to store.

5. Render
- Build derived viewmodels.
- Render all tabs under signal-blocked repaint sections.

6. Activate
- Enable timers, media polling, and background workers only after first full render completes.

---

## Speakers/BoxStream Dataflow (Target)
1. Intent: `IdentifyUniqueFaces(clipId, options)`
2. Effect: run candidate detection/tracking and populate identity-resolution table.
3. Reducer: persist resolved unique-face mapping for all distinct faces.
4. Intent: `GenerateBoxStream(clipId, preset, options)` (allowed only after unique-face pass is resolved).
5. Reducer: mark job state `running`.
6. Effect: run selected backend (native/docker/python/sam3).
7. Effect result intent: `BoxStreamGenerated(payload)` or `BoxStreamFailed(error)`.
8. Reducer:
- validate schema
- update boxstream sidecar refs
- update speaker tracking metadata
9. Store commit + persistence schedule.
10. UI render:
- update table from derived viewmodel only
- preserve selection if possible, else pick first row once

---

## Engineering Rules
1. No state mutation inside widget selection-change handlers except dispatching intents.
2. Every refresh function must be:
- idempotent
- reentrancy-guarded
- signal-guarded during bulk widget changes
3. Heavy operations must not run inline on UI event handlers.
4. Persistence writes are effect-layer responsibilities only.
5. Debug artifacts must be opt-in retention with cleanup tooling.

---

## Observability and Regression Checks
1. Add counters/timers
- refresh call counts per panel
- reentrancy guard hit count
- render duration percentiles
- history size and entry count

2. Add startup profile checkpoints
- project load time
- history parse time
- first render completion time

3. Add watchdog assertions (debug builds)
- detect nested render depth > 1 where prohibited
- detect repeated same-intent flood in short window

4. Add tests
- unit: reducer determinism and schema validation
- integration: startup with large history + boxstream artifacts
- UI smoke: table repopulation does not recurse

---

## Rollout Plan
1. Week 1
- Land Phase 0 globally across heavy panels.
- Add telemetry counters.

2. Week 2
- Implement Phase 1 intent routing + debouncing for speakers/transcript paths.

3. Week 3
- Implement Phase 2 split (`compute*Model`/`render*Model`) for SpeakersTab.

4. Week 4
- Implement Phase 3 history caps/dedupe + cleanup automation.

5. Week 5
- Implement Phase 4 worker isolation and schema normalization for detector backends.

---

## Definition of Done
- Startup reaches first interactive frame without CPU runaway.
- No recursive refresh traces under normal workflows.
- `history.json` size remains bounded over extended editing sessions.
- Heavy tracking pipelines do not block UI thread.
- Dataflow rules documented and enforced in code review.

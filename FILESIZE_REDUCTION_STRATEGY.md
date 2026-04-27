# File Size Reduction Strategy

## Goals

1. Keep every `.cpp` and `.h` file under **1500 lines**.
2. Keep a soft cap of **800-1200 lines** for active files.
3. Make file ownership obvious and stable.
4. Reduce merge conflicts by splitting high-churn responsibilities.

## Enforcement

1. Hard rule: no new or modified file may end above `1500` lines.
2. CI policy:
   - warn at `>1200`
   - fail at `>1500`
3. PR policy:
   - if touching a file over 1500, include a split in the same or immediate next PR.

## Decomposition Standards

1. Split by responsibility, not by arbitrary line ranges.
2. Keep orchestration in parent files and move heavy logic to leaf files.
3. Use stable seams:
   - domain (`ai`, `speaker`, `timeline`, `render`, `control_server`)
   - role (`layout`, `bindings`, `state`, `actions`, `routes`)
4. Prefer move-only extraction first, behavior refactors second.
5. Keep function signatures stable during extraction where possible.
6. Keep headers narrow:
   - public API in `*.h`
   - private/shared implementation details in `*_internal.h`
7. Keep cross-file APIs small and explicit.
8. Avoid mixed concerns in one file (for example, UI layout + networking + persistence).

## Naming Standards

1. Use `domain_subject_role.cpp` and `domain_subject_role.h`.
2. UI splits:
   - `*_layout.cpp`
   - `*_bindings.cpp`
   - `*_state.cpp`
   - `*_actions.cpp`
3. Route splits:
   - `*_routes_<area>.cpp`
4. Avoid ambiguous names (`misc`, `helpers2`, `new`).

## Organization Scheme (What Code Belongs Where)

Use this scheme whenever splitting a large file.

1. `*_main.cpp`
   - app startup/shutdown, `main()`, top-level runtime bootstrap only.
2. `*_cli.cpp`
   - command-line parsing, CLI option validation, CLI dispatch.
3. `*_layout.cpp`
   - widget/view construction, layout hierarchy, static UI structure.
4. `*_bindings.cpp`
   - signal-slot wiring, event subscriptions, callback hookup.
5. `*_state.cpp`
   - model-to-UI sync, UI-to-model sync, state refresh/apply, serialization glue.
6. `*_actions.cpp`
   - user-triggered commands, mutations, command orchestration.
7. `*_auth.cpp`
   - login/logout/session/token lifecycle, entitlement checks.
8. `*_routes_<area>.cpp`
   - HTTP/control-server route handlers grouped by area (`health`, `state`, `ui`, `media`, `debug`).
9. `*_pipeline_<stage>.cpp`
   - multi-stage processing flows split by stage:
     - input/normalize
     - detect/candidate generation
     - match/classify
     - apply/writeback
     - debug/artifact emission
10. `*_io.cpp` or `*_storage.cpp`
   - filesystem/network persistence, load/save logic, cache storage internals.
11. `*_internal.h`
   - private shared structs/constants/helpers used by multiple implementation files in one domain.
12. `*.h` (public)
   - minimal API surface only; no heavy private implementation details.

### Ownership Rule

Each function should have one primary home:

1. If it builds UI, it belongs in `*_layout.cpp`.
2. If it wires events, it belongs in `*_bindings.cpp`.
3. If it mutates domain state due to a user command, it belongs in `*_actions.cpp`.
4. If it only synchronizes state or applies snapshots, it belongs in `*_state.cpp`.
5. If it handles auth/session/entitlements, it belongs in `*_auth.cpp`.
6. If it serves an endpoint, it belongs in `*_routes_<area>.cpp`.
7. If it is stage-specific processing, it belongs in `*_pipeline_<stage>.cpp`.

## Refactor Workflow

1. Pick one source file and define target destination files by responsibility.
2. Add destination files to build system first.
3. Move code in small compile-safe batches.
4. Build and run focused tests after each batch.
5. Remove dead code and duplicate declarations.
6. Repeat until source file is below threshold.

## Definition of Done

1. Source file is below 1500 lines.
2. Destination files each have one clear ownership area.
3. Build passes.
4. Relevant tests pass.
5. No net behavior change unless explicitly intended.

## Repo-Specific Plan

Repo-specific source/destination mappings and execution order are maintained in `REFACTOR_PLAN.md`.

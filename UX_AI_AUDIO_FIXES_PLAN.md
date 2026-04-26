# UX/AI/Audio Fixes Plan

## Objective
Deliver a cohesive upgrade across grading UX, AI-assisted metadata workflows, and audio preview/tooling without destabilizing render/export reliability.

## Scope
1. Improve Grading Tab UX polish.
2. Add bottom-right AI integration panel, gated by Supabase/Stripe setup in `../QTSynth`.
3. Add AI actions for speaker/entity cleanup.
4. Add switchable preview mode for accurate waveform/audio tooling (normalization, peak reduction, limiter, compressor, and related core controls).

## Non-Goals (Phase 1)
- Full DAW-grade multiband mastering.
- Automatic destructive rewrite of source media files.
- Replacing existing render pipeline architecture.

## Guiding Principles
- KISS: one clear path for each user action.
- Non-destructive editing: all audio tools become clip/track parameters and keyframes, not source rewrites.
- Progressive disclosure: basic controls first, advanced settings in expandable sections.
- Fast feedback: preview updates should be near real-time on short segments.

## Workstream A: Grading Tab UX Polish

### A1. Information Architecture Cleanup
- Group controls by intent: `Exposure`, `Color Balance`, `Curves`, `Matching/Assist`, `Scopes`.
- Keep histogram + channel selector persistently visible.
- Ensure no overlapping tabs/panels and no hidden controls due to layout shifts.

### A2. Interaction Polish
- Consistent slider/input ranges on normalized scale (0..1 or documented signed ranges).
- Add reset buttons at section level and per control.
- Improve keyframe affordances: explicit on/off state, current-frame indicator, and conflict-free toggling.

### A3. Visual Consistency
- Improve spacing, alignment, and typography hierarchy.
- Harmonize selected states and borders (especially clip/speaker selection visibility).
- Keep rendering preview overlays readable against bright/dark content.

### A4. Performance Hygiene
- Throttle expensive redraws for histogram/curve updates.
- Avoid high-frequency rich-text relayout in render progress UIs.

### Deliverables
- Updated Grading tab layout and controls.
- UI polish checklist with before/after screenshots.

## Workstream B: Bottom-Right AI Panel (Supabase/Stripe Gated)

### B1. Entitlement and Config Gate
- Add `AiAccessGate` abstraction that reads integration status from `../QTSynth`.
- Gate availability on:
  - Supabase session/auth valid.
  - Stripe entitlement/subscription allows AI features.
- If unavailable, show disabled panel with clear reason and setup CTA.

### B2. AI Panel UX
- Dock panel bottom-right in the editor shell.
- Include model selector (`bmodel` / base model) with default to `DeepSeek`.
- Include cheap/professional fallback models (config-driven) with labels for cost/speed/quality.

### B3. Model Strategy (Professional Practice)
- Default model: DeepSeek (cost-efficient default).
- Additional budget options: low-cost LLMs from provider config.
- Keep model list externally configurable (from QTSynth/env/config JSON), not hardcoded.
- Record model choice per project and allow per-action override.

### B4. Context Access
- AI requests can access safe project context bundle:
  - Media hierarchy (project -> tracks -> clips -> speakers).
  - Transcript metadata and speaker segments.
  - Quality signals (confidence, durations, orphan assignments).
- Explicit opt-in for cloud calls if sensitive media metadata is present.

### Deliverables
- `AI` panel in bottom-right.
- Entitlement gate and status UI.
- Model registry + default/fallback behavior.

### B5. QTSynth Contract Definition (Required)
- Define and version the integration contract with `../QTSynth`:
  - Endpoint paths and methods.
  - Request/response schemas.
  - Auth token/session flow.
  - Error codes and retryable vs non-retryable failures.
- Add a compatibility check at startup and surface clear mismatch diagnostics.

## Workstream C: AI Speaker/Organization Cleanup Actions

### C1. New AI Actions
Add buttons:
- `Find Speaker Names (AI)`
- `Find Organizations (AI)`
- `Clean Spurious Speaker Assignments`

### C2. Behavior Specs
- Name discovery: infer likely person names from transcript context and repetition.
- Organization discovery: infer affiliated organizations/entities.
- Spurious assignment cleanup:
  - Detect one-off speaker labels with low confidence/short duration.
  - Propose merge/reassign/delete suggestions.
  - Require user confirmation before applying.

### C3. UX + Safety
- Show confidence and rationale per suggestion.
- Batch apply with undo support.
- Keep an audit trail in project history.

### C4. Transcription Pipeline Details (Required)
- Specify transcription modes: batch and optional streaming.
- Define diarization strategy and language handling/fallback.
- Add retry policy, timeout policy, and degraded-mode UX when AI is unavailable.

### C5. Cost and Safety Controls (Required)
- Per-project usage budget/cap and optional hard stop.
- Request rate limiting and concurrency caps.
- Explicit model fallback order and timeout thresholds.
- User-visible usage telemetry in AI panel.

### Deliverables
- Three action buttons wired to AI service.
- Review/apply dialog with confidence and undo.

## Workstream D: Preview Window Audio View + Dynamics Tools

### D1. Preview Mode Switch
- Add `Video | Audio` mode toggle in preview window.
- In `Audio` mode, render accurate waveform for selected track/clip/time range.

### D2. Waveform Accuracy Requirements
- Use decoded PCM at project sample rate.
- Multiresolution cache (overview + zoom detail).
- Proper channel visualization (L/R or summed, user-selectable).
- Time alignment must match timeline frame/sample mapping exactly.

### D3. Core Audio Tools (Non-Destructive)
- Normalization (peak and loudness-target options).
- Peak reduction (fast transient control).
- Limiter (true-peak aware where possible).
- Compressor (threshold/ratio/attack/release/makeup).
- Optional essentials: high-pass filter and noise gate (if low risk to add).

### D4. Monitoring and Metering
- Add peak and loudness meters (LUFS short-term/integrated where feasible).
- Preview bypass toggle per effect and global bypass.
- Effect chain order visible and editable.

### D5. Audio Processing Architecture (Required)
- Define canonical effect chain order and where each tool runs.
- Specify parameter ranges/default values and units.
- Add lookahead/latency compensation policy.
- Define true-peak estimation method and clipping safeguards.

### Deliverables
- Audio preview mode with waveform.
- Audio dynamics tool panel with live preview and meters.

## Architecture and File-Level Plan

### New/Updated Components
- `grading_tab.cpp/.h`: UX re-layout and control polish.
- `editor.cpp/.h` and `editor_setup.cpp`: dock AI panel bottom-right; add preview mode switch plumbing.
- `preview_window_*.cpp` + `preview.h`: audio mode rendering and waveform draw path.
- `audio_engine.h` + new `audio_fx_chain.*`: dynamics processing chain.
- `speakers_tab*.cpp`: AI speaker/org actions and cleanup workflows.
- `control_server_worker_routes.cpp`: REST endpoints for AI actions and audio tool params.
- New docs:
  - `AI_INTEGRATION.md`
  - `AUDIO_PREVIEW_AND_DYNAMICS.md`
  - update `UI_LAYOUT.md`.

### Integration Boundary with `../QTSynth`
- Shared contract for auth/entitlements:
  - Session token retrieval.
  - Stripe entitlement check API.
  - Model registry/config lookup.
- Fail closed if contract unavailable; show explicit diagnostics in UI.

### Privacy and Compliance Controls (Required)
- Explicitly define what project/media metadata may leave device.
- Data retention/deletion policy for AI requests and responses.
- Redaction options for sensitive text/entities before cloud calls.
- Consent UX and per-project opt-in state persistence.

### Background Jobs and Reliability (Required)
- Introduce queued background jobs for AI/transcription/audio analysis tasks.
- Define cancel/resume/retry semantics and progress surfaces.
- Ensure long-running jobs cannot block the UI thread.

### Undo/Audit Schema (Required)
- Persist proposed AI changes separately from applied edits.
- Record who/what/when for each applied suggestion.
- Guarantee deterministic rollback for batch apply operations.

## Execution Phases

### Phase 0: Contract + Guardrails (1-2 days)
- Define QTSynth entitlement/config contract.
- Add stubs and feature flags.
- Add diagnostics panel/logging for gate state.
- Define privacy/compliance constraints and consent flow.

### Phase 1: Grading UX Polish (2-3 days)
- Implement layout and interaction polish.
- Resolve clipping/overlap and normalize ranges.
- Add UI regression screenshots.

### Phase 2: AI Panel + Model Selector (2-3 days)
- Build docked panel and entitlement gate.
- Add model registry + default DeepSeek.
- Implement base request wiring and error handling.

### Phase 3: AI Speaker/Org Tools (2-4 days)
- Add actions and review dialog.
- Add apply/undo/audit path.
- Add REST endpoints and tests.

### Phase 4: Audio View + Dynamics (4-6 days)
- Add waveform mode and cache.
- Add normalization/limiter/compressor/peak reduction controls.
- Validate timeline sync and preview latency.

### Phase 5: Stabilization (2-3 days)
- Performance profiling on render and preview paths.
- Crash/freeze regression checks.
- Final docs and acceptance checklist.

### Phase 6: Rollout + Migration (1-2 days)
- Add staged feature flags (AI panel, AI actions, audio view, audio FX).
- Add migration handling for existing project files and defaults.
- Define go/no-go criteria for enabling by default.

## Acceptance Criteria
- Grading tab has no overlap/clipping and consistent normalized controls.
- AI panel appears bottom-right and is clearly gated by Supabase/Stripe state.
- Model selector defaults to DeepSeek and supports configured cheap alternatives.
- AI actions can propose speaker names, organizations, and cleanup candidates with confidence and user-confirmed apply.
- Preview can switch to audio mode; waveform aligns to timeline accurately.
- Audio tools produce audible/visible effect in preview and survive save/load.
- No regression in standard render/export path.
- QTSynth contract version check passes (or clearly fails closed with actionable message).
- AI usage limits/rate limits/fallback behavior are enforced and observable.
- Background AI tasks are cancelable and never freeze UI.
- Undo/audit trail is present for all AI-applied modifications.
- Accessibility baseline passes for new controls (keyboard/focus/contrast).

## Test Plan
- Unit tests:
  - Entitlement gate logic.
  - QTSynth contract parser/version compatibility checks.
  - Model registry default/fallback selection.
  - Speaker cleanup heuristics.
  - Audio FX parameter validation.
  - Undo/audit persistence and rollback behavior.
- Integration tests:
  - REST endpoints for AI actions.
  - Transcription batch flow and failure fallback flow.
  - Save/load persistence for AI/audio settings.
- UI tests/manual scripts:
  - Panel docking and layout stability.
  - Audio/video mode switching.
  - Undo/redo for speaker cleanup.
  - Accessibility pass for keyboard and focus traversal.
- Performance:
  - CPU usage under active waveform + effects.
  - Render progress responsiveness without UI lockups.
  - Memory ceilings with long timeline + waveform cache.
  - Target interaction latency under AI panel activity.

## Performance SLOs (Required)
- UI interaction latency: < 100 ms for primary control changes.
- Preview update budget (audio mode): maintain interactive scrubbing on representative projects.
- Memory budget for waveform cache: bounded and configurable.
- Background AI work: no main-thread stalls > 16 ms attributable to AI polling.

## Accessibility Baseline (Required)
- Full keyboard access for AI panel and audio controls.
- Visible focus indicators on all actionable controls.
- Contrast compliance for new labels/overlays/states.
- Tooltips/help text for advanced audio parameters.

## Rollout and Migration (Required)
- Feature flags:
  - `feature_ai_panel`
  - `feature_ai_speaker_cleanup`
  - `feature_audio_preview_mode`
  - `feature_audio_dynamics_tools`
- Migration:
  - Add defaults for new project fields.
  - Preserve backward compatibility for older project files.
  - Provide one-time migration diagnostics in logs.

## Risks and Mitigations
- Risk: QTSynth contract drift.
  - Mitigation: versioned contract + graceful capability fallback.
- Risk: AI false positives on names/entities.
  - Mitigation: confidence thresholds + mandatory review/apply.
- Risk: audio processing causes latency spikes.
  - Mitigation: block-size tuning, caching, and optional quality tiers.
- Risk: UI complexity creep.
  - Mitigation: KISS review gate before merge of each workstream.

## Definition of Done
- All acceptance criteria pass.
- Regression suite green.
- New docs merged and linked from `README.md` or `ARCHITECTURE.md`.
- Feature flags documented with defaults and rollout strategy.

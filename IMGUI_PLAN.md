# Dear ImGui UI Parity Plan

## Goal

Provide a Dear ImGui implementation of JCut that can eventually replace the Qt shell without
forking editor behavior or project data.

The Qt and ImGui frontends must reuse the same editor/runtime code, project directory, active
project marker, `state.json`, and `history.json`. UI frameworks should own presentation and input;
editing rules, serialization, rendering, export, and history belong in shared services.

## Current Status (2026-07-23)

The ImGui editor is now a substantially more capable migration checkpoint, but it is **not yet
fully on par with the Qt editor**.

The core editing loop, safe project interchange, undo/redo, a broad set of inspector bindings,
preview, and standalone export are implemented. Advanced editor workflows and rich-render
equivalence remain incomplete; strict Qt-free linkage of the final `jcut_imgui` executable is now
complete and guarded by automated checks.

The ImGui shell now talks to preview audio, preview rendering, and external Vulkan frames only
through Qt-free public implementations. Preview audio uses RtAudio plus the standalone decoder,
mixer, and shared Rubber Band core; it retains asynchronous warmup, source polling, sidecar
discovery, mute/solo/gain/pan/fade policy, and neutral status reporting. The external Vulkan
importer uses the shared Qt-free core used by both frontends. Preview currently uses the standalone
CPU timeline renderer; restoring equivalent rich composition and a Qt-free Vulkan producer is now
a functional/render-quality task rather than a binary-linkage task.

Automatic speaker framing now has a neutral durable contract rather than surviving only in opaque
Qt legacy state. Enablement, baked target metadata, manual selectors, smoothing/gap settings, and
enabled/framing/target keyframes round-trip through neutral JSON and both Qt bridges. Qt and
standalone preview/export share `speaker_framing_core.h` for baked interpolation, retargeting, and
face-box transforms, and `speaker_framing_smoothing_core.h` for robust center/zoom smoothing.
Standalone rendering applies baked framing and transcript-profile/manual/section/identity
continuity samples across the persisted frame domains, including gap hold and section-rotation
fallback. ImGui's Transform inspector edits every scalar setting plus enabled, baked, and target
keyframes through undoable shared commands. The durable automatic-framing runtime and authoring
contract is therefore complete; remaining preview parity lies in rich-composition/GPU production
rather than framing-state divergence. Transcript-derived primary/secondary/accent speaker palettes
also now drive standalone speaker-mask effects with the same Qt fallback colors.

Prompt-mask creation is no longer a Qt-dialog-only workflow. Qt and ImGui share
`prompt_mask_job_core` for SAM3 prompt/path sanitization, source-adjacent artifact naming,
optimization arguments, environment/cache routing, resumable manifests, cancellation, and
validated sidecar results. The ImGui Masks tab exposes the Qt functional preflight options,
supports resume or explicit restart, reports the worker in Jobs, and atomically materializes the
completed selected/union sidecar through the shared editor command. Qt retains its native dialog
presentation but now builds its paths and launch command from the same neutral plan.

Interactive transcription and BiRefNet alpha generation are also no longer Qt-only process
workflows. A shared Qt-free subprocess controller owns process-group lifetime, merged bounded log
capture, stdin delivery, cooperative cancellation with a forced timeout, and thread-safe status.
Qt and ImGui reuse neutral WhisperX and BiRefNet plans for source-adjacent job roots, commands,
cache/environment routing, manifests, progress, and result discovery. ImGui exposes real
transcription launch/stdin/cancel/artifact inspection and BiRefNet model/revision/device/precision/
tolerance/resume-restart controls, progress/cancel diagnostics, and successful matte
materialization through the same editor command used by other generated sidecars. ImGui polls the
same atomically replaced live-preview artifact as Qt at 250 ms, decodes it through the standalone
Qt-free media decoder, and presents it through an independently owned Vulkan texture so worker
updates cannot replace the Program monitor descriptor.

AI account access is no longer an empty ImGui surface. `ai_gateway_core` is a Qt-free libcurl
client for the same `/api/ai/entitlements`, `/api/ai/request`, `/license`, and `/api/ai/task`
contracts used by the Qt `CPPMonetize` adapter, including the existing Supabase-direct
`ai_request`/`deepseek_chat` fallback. It owns gateway normalization, entitlement/usage/license
parsing, model/limit projection, access-table rows, task submission, and response-text extraction;
Qt's URL helper now delegates to the same normalization rule. ImGui refreshes entitlement and
usage asynchronously, renders real subscription/purchase/access rows, offers model-backed
stateful chat with project context, enforces the persisted project request budget, and preserves
the Qt-compatible gateway/model/feature/counter fields. A Qt-free credential-store adapter uses
the same `jcut.ai.auth`/token/refresh/user secret attributes as Qt on Linux, with the same
permission-restricted `PanelTalkEditor` config-file fallback; ImGui loads, explicitly saves, and
clears those credentials across frontend switches. Chat also applies the entitlement-provided
one-minute rate window, ordered model fallback, timeout, and bounded retry policy. Bearer tokens
may also remain memory-only or come from `JCUT_AI_AUTH_TOKEN`; they are never written to project
state. A cancellable Qt-free Supabase browser-PKCE flow binds a loopback-only ephemeral callback,
generates an RFC 7636 S256 challenge, launches the system browser, exchanges the returned code,
and saves the resulting session through the shared store. Stored Supabase refresh tokens can also
be exchanged asynchronously through the neutral client,
saved back to the shared store, and are used automatically after an entitlement authentication
failure. ImGui also reuses Qt's configured/fallback product-slug order to create and open
subscription checkout asynchronously. Shared cloud speaker payload/response projection turns
model-inferred names and organizations into the same explicitly reviewed neutral proposals used
by deterministic mining; ImGui applies selected results through transcript undo. Deterministic
assignment cleanup already shares the same core in both shells. Remaining differences in this
area are final native account/activity presentation rather than disconnected service behavior.
Qt and ImGui now also share the Qt-free access-token profile parser for email/user/avatar claims.
ImGui fetches HTTP(S)-only avatar images asynchronously with a 4 MiB bound, decodes them through
the standalone media service, presents them on an independently owned rounded Vulkan texture with
the entitlement-colored ring, and falls back deterministically to initials. Temporary image bytes
are never stored in project state and are removed at shutdown.

The built `jcut_imgui` executable now passes the strict all-Qt linkage check. Its CMake chain uses
the Qt-free editor core, standalone preview/export/audio services, Vulkan importer, RtAudio,
FFmpeg, X11, and Vulkan; Qt6 Gui/Core/DBus as well as Widgets/Network/Concurrent are absent. This
closes the binary-boundary milestone, but is not evidence of full UI or rich-render parity.

The complete unfiltered repository build now passes with `./build.sh`. Two dependency regressions
introduced while splitting the ImGui runtime boundary were corrected before accepting that
checkpoint: the offscreen face-detection executable now links its direct `Qt6::Network`
dependency for `QLocalSocket`, and `startup_project_state.cpp` lives with the widget-facing
`ProjectManager` implementation instead of creating a static-library dependency cycle through
`jcut_runtime_support`. The optional Qt CorePrivate/GuiPrivate discovery warnings remain benign.
Passing this build gate establishes build integrity only; it does not establish UI, preview,
audio, or export parity.

This document is the migration guideline and the working parity ledger. “Implemented” means the
feature is connected to the same durable project/transcript/artifact contract as Qt, has relevant
automated coverage, and survives the repository build gates below. A control that is merely drawn,
a focused target that compiles, or a one-off manual success is not sufficient evidence. Until every
row in the parity matrix has no material remainder and the exit criteria pass, the accurate answer
to “is ImGui at full Qt parity?” remains **no**.

### Build-triage rule

- Always reproduce with the repository wrapper and continue from the first fatal diagnostic:
  `./build.sh` for the acceptance gate, or `./build.sh --with-tests --target <target>` while
  narrowing a failure. Do not treat optional-package discovery warnings as the stopping error.
- The recurring `Could NOT find Qt6CorePrivate` and `Qt6GuiPrivate` messages are optional
  configuration warnings on this system. They are not the cause of a later failed compilation.
- The reported offscreen face-detection failure was the fatal `QLocalSocket: No such file or
  directory` diagnostic. Those sources directly include `QLocalSocket`, so their executable must
  compile and link with `Qt6::Network`; relying on a transitive Qt dependency is invalid.
- A focused target passing is only an intermediate result. Work is not accepted until the exact
  unfiltered `./build.sh` command succeeds, followed by the relevant focused tests and the strict
  no-Qt check for `build/jcut_imgui`.
- Treat timing- or scheduler-sensitive signal assertions as test-quality issues only after the
  implementation passes repeated isolated runs and the measured result remains inside the intended
  engineering tolerance. The shared Rubber Band 440 Hz preservation check now uses a symmetric
  390-490 Hz zero-crossing band, avoiding a false failure at the former strict 400 Hz boundary
  without weakening the test enough to accept the unpreserved 880 Hz result.
- Stop at the first actionable compiler/linker/test failure, fix that root cause, and rerun the
  same wrapper command. Do not spend repeated long runs past a deterministic failure or mistake
  optional package warnings for the fatal diagnostic.
- Record the exact wrapper command and the first fatal diagnostic in handoff/status updates. This
  keeps long autonomous runs auditable and prevents a partially built tree from being mistaken for
  a completed migration.

## Reuse Strategy

The implementation deliberately extends existing code instead of creating ImGui-specific
business logic:

- `EditorDocumentCore` is the neutral document exchanged by the shell, runtime, project adapter,
  preview, and export paths.
- `EditorRuntime` owns editor commands, selection, transport, undo/redo, and history transaction
  behavior used by the ImGui controls.
- The legacy Qt state bridge preserves fields the neutral model does not yet understand and
  reuses existing render timeline and effect-preset conversion logic.
- ImGui project I/O operates directly on the Qt project store and history formats.
- ImGui reads and writes Qt's project-level autosave interval, backup retention, history-entry,
  and history-size fields. Periodic backups reuse the legacy-state bridge, preserve unknown fields,
  use Qt-compatible `state_backup_*.json` names, avoid mutating state/history, and trim oldest
  backups under the shared project save lock.
- Decoder preference and H.264/H.265 software-threading policy now live in the Qt-free
  `jcut_decoder_policy_core`. The Qt debug adapter delegates its string conversion/parsing to that
  core, while standalone ImGui preview, proxy generation, export, and the decode benchmark apply
  the same normalized policy. A shared Qt-free FFmpeg helper selects hardware pixel formats and
  creates platform hardware contexts; standalone preview, proxy generation, export, and benchmarks
  now attempt CUDA/VAAPI (or native platform equivalents), transfer decoded frames into the current
  CPU compositor, and reopen in software when device creation, codec opening, or frame decode
  fails. ImGui reports requested versus effective mode, active device, and the exact fallback.
  The hardware device preference is persisted independently of project content and supports Auto,
  CUDA, VA-API, VideoToolbox, D3D11VA, and DXVA2.
- A Qt-free CUDA hardware-direct preview slice is implemented for eligible single-layer frames.
  The standalone decoder retains its NV12 or RGBA/BGRA CUDA `AVFrame` in `FramePayloadCore`
  without calling `av_hwframe_transfer_data`. `VulkanHardwareFrameImportCore` copies device to
  device into Vulkan-exported memory, reuses the existing NV12-to-RGBA compute shader, and exposes
  a sampled image to ImGui. Any eligibility, device, allocation, CUDA import/copy, shader, or
  Vulkan submission failure requests a CPU-composited rerender of the same preview generation and
  reports the exact reason. Active transcript overlays keep the video frame direct: the shared
  layout/rasterizer produces only a transparent, alpha-bounds-cropped subtitle layer, which ImGui
  uploads into a reusable auxiliary Vulkan texture and blends in output space. Overlay upload
  failure requests a complete CPU rerender, so subtitles are never silently omitted. The
  generated-proxy export test exercises retained hardware decode and
  the importer when CUDA is active on the test host; software-only hosts exercise the explicit
  fallback branch. Evaluated translation/rotation/scale/source-transform chains and opacity stay
  direct as ImGui presentation metadata. Base brightness/contrast/saturation stays direct for
  NV12 CUDA surfaces by applying the shared grading order in the conversion shader. Every visual
  track above the direct base video now reuses the complete standalone renderer on a transparent
  canvas, so upper videos, images, titles, masks, effects, corrections, grading, and transforms
  merge with transcript pixels into one cropped auxiliary Vulkan overlay. A failed upper-layer
  decode or render rejects the hybrid result and requests full CPU composition. VA-API import and
  effects/masks/corrections on the hardware base layer remain unfinished; upper-layer and
  title/subtitle rasterization is still CPU-side even though composition no longer transfers the
  base video frame.
- Decoded hardware-frame presentation must use `FramePayloadCore` as the framework-neutral
  lifetime contract. The decoder clones the selected FFmpeg `AVFrame` into that payload before
  releasing its receive buffer; workers and UI code exchange `shared_ptr<FramePayloadCore>`
  rather than `FrameHandle`, `QImage`, or raw `AVFrame*`. The payload is immutable after
  publication and remains alive until the render thread has finished importing/copying it.
- The existing CUDA-to-Vulkan implementation in `VulkanDetectorFrameHandoff` supplied the proven
  interop algorithm for the new Qt-free `VulkanHardwareFrameImportCore`; no interop code lives in
  `jcut_imgui_main.cpp`. ImGui and the Qt detector/direct-render entry points now delegate their
  active hardware-direct path to the neutral core. The legacy class still owns its Qt CPU-upload
  and command-recording compatibility surface, but no longer owns the active CUDA import path.
- Hardware decode, hardware-frame publication, and hardware-direct presentation are three
  independently reported states. “Hardware accelerated” may describe decode even when a CPU
  transfer was required. “Hardware direct” or “zero copy” may be reported only when the selected
  frame reached the ImGui Vulkan image without `av_hwframe_transfer_data` or a CPU staging upload.
  Diagnostics must name the requested decoder, active FFmpeg device, retained hardware pixel
  format/software format, selected handoff path, and exact fallback reason.
- The direct-presentation slice accepts one active ordinary visual clip with no
  mask/effect/correction operation and no generated matte role as its lowest active decoded layer.
  Every higher track is transparently subcomposited with the normal renderer; active layers at or
  below the chosen base are rejected when they cannot preserve ordering. Base/keyed
  translation, rotation, scale, source-transform chains, and opacity remain direct: the renderer
  carries their evaluated metadata with the retained hardware frame and ImGui presents it as a
  clipped, alpha-tinted Vulkan image quad. Base brightness/contrast/saturation also remains direct
  for NV12 CUDA frames; other graded hardware formats fail safely into the CPU compositor.
  Baked and transcript/continuity-driven speaker framing now reuse the same resolved presentation
  transform in CPU and hardware-direct rendering, and the force-opaque track mode stays direct by
  overriding presentation opacity without changing project state. Rich
  timeline frames continue through the compositor until the shared Vulkan renderer can reproduce
  every enabled operation. Eligibility is a shared renderer decision, not an ImGui-shell guess.
- Direct presentation is optimistic and recoverable. If the render thread rejects the payload
  because the Vulkan device lacks required external-memory/semaphore extensions, the FFmpeg
  surface type is unsupported, CUDA and Vulkan refer to incompatible devices, shader resources
  cannot be created, or synchronization/import fails, the shell requests a fresh CPU-composited
  frame for the same generation. It must never display a stale hardware surface or silently label
  a CPU fallback as zero-copy.
- Hardware handoff resources are render-thread owned and device-scoped. Decoder payloads retain
  FFmpeg surface lifetime only; Vulkan images, exported allocations, CUDA imports, descriptors,
  pipelines, semaphores, and deferred-retirement queues are released before the Vulkan device.
  Decoder-policy changes, project switches, preview-size changes, swapchain/device recreation,
  and shutdown must drain or invalidate outstanding work deterministically.
- The Qt-free RtAudio facade owns output-device discovery and stream selection. ImGui persists the
  stable device name (or system-default choice), can refresh the available stereo-output list,
  restarts an open stream safely after selection, and reports requested versus active device names
  together with requested/actual buffer frames.
- Project-level audio dynamics now use `DynamicsSettingsCore` in both frontends. Qt's preview
  settings type aliases the neutral model, and its audio engine delegates sample processing to the
  same Qt-free DSP used by standalone ImGui preview and export. Amplification, whole-buffer and
  selective normalization, peak reduction, compression, soft clipping, limiting, and stereo-to-
  mono follow the established Qt processing order and bounds. ImGui edits the functional controls
  through one undoable runtime command; both neutral JSON and Qt's exact `audio*` root keys
  round-trip without losing the waveform/transcript-only flags that are not yet exposed.
- Qt's two-stage Rubber Band Harmonic Speech Isolation treatment now lives in the shared Qt-free
  stretch core. The Qt wrapper delegates to it, the neutral document preserves Qt's exact legacy
  `playbackAudioWarpMode`, and ImGui selects it through undoable runtime history for standalone
  preview/export decoding.
- Transcript normalization is functional in the standalone path: the neutral cut-session loader
  resolves each clip's active cut, maps eligible word ranges into canonical source samples,
  measures the decoded per-word peak, applies Qt's 0.95 linear target and 2.5x cap, and uses the
  same 10 ms boundary fades/120 ms inter-word gain bridge during preview/export mixing. The
  persisted checkbox is now exposed through the shared dynamics command.
- Standalone export reuses the neutral document/render contracts and FFmpeg backend.
- The existing Qt editor remains buildable while boundaries are extracted incrementally.
- Proxy association is a shared neutral concern: default sidecar discovery, usability checks,
  attach/use/disable/clear behavior, generated mask-child cache normalization, and undo/redo live
  outside both UI shells. ImGui image-proxy generation is also a Qt-free job built on the existing
  standalone timeline renderer and FFmpeg encoders; JPEG sequences, H.264 MP4, and Motion JPEG MOV
  have explicit overwrite consent, progress, cancellation, manifests, and completed-result attach.
  JPEG continuation resumes after the highest generated frame. Confirmed managed deletion is
  restricted to the three source-adjacent proxy candidates and rejects symlinks.
- Long-running processing remains out of the UI layer. ImGui must launch the existing
  `jcut_vulkan_facedetections_offscreen` executable with the same command-line options, job
  manifest, resumable checkpoint, and final artifact formats used by Qt. A Qt-free controller may
  own process lifetime and UI-safe status snapshots, but it must not duplicate detector,
  continuity, or artifact-generation algorithms.
- Face-generation import must produce the same transcript-adjacent `JCUTBOX1` continuity document
  consumed by Qt and by `FaceArtifactInspectionCore`. Generated sidecar paths, frame domains,
  detector metadata, clip identity, and unknown artifact fields must be preserved.
- Cancellation is cooperative first and forced only after a bounded grace period. A canceled face
  job is `paused`, not falsely `completed` or destructively reset; `facedetections.part` and its
  resume index remain available unless the user explicitly requests “restart from scratch.”
- Generic external workers use `process_job_core`; WhisperX and BiRefNet layer only their durable
  command/artifact contracts on top. Frontends may retain native presentation, but must not
  independently reconstruct command lines, process groups, cancellation semantics, or result
  readiness.

Duplicating command semantics or serialization rules in `jcut_imgui_main.cpp` is considered a
migration bug.

### Hardware-frame acceptance evidence

- A neutral-boundary test must construct/clone hardware-frame metadata without linking any Qt
  library and verify ownership, move/lifetime, pixel-format, crop, and memory-accounting behavior.
- Renderer tests must prove the single-layer eligibility decision accepts presentation-only
  transform/opacity changes while rejecting every operation that still requires pixel
  composition.
- A runtime test with an available CUDA decoder must prove one of two explicit outcomes: an
  `AV_PIX_FMT_CUDA`/NV12 payload is presented through the shared Vulkan handoff with no CPU
  transfer, or the result contains a non-empty, actionable fallback reason and a valid CPU frame.
  Machines without compatible CUDA/Vulkan interop may take the second branch; they may not skip
  validating the fallback contract.
- The current generated H.264-compatible proxy test uses a 64x64 source so NVIDIA decode can
  produce a supported hardware surface. When the decoder reports CUDA active, it retains the
  frame and requires `VulkanHardwareFrameImportCore` to return a valid
  `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` sampled image. If hardware decode is unavailable, it
  requires the existing named software-fallback contract instead.
- Completed: carry evaluated base/keyed/source-linked transform and opacity metadata with eligible
  single-layer retained hardware frames, project it through a shared tested output-to-screen quad
  helper, and render the imported Vulkan image with ImGui vertex alpha over the same neutral
  compositor background color without CPU pixel staging.
- Completed: extend the CUDA NV12 conversion push contract and compute shader with the neutral
  shadow/midtone/highlight then brightness/contrast/HSL-saturation order. The shared evaluator
  supplies animated basic and tonal values, so grading keyframes remain direct without duplicating
  interpolation in the shell. A shared packed 256-entry RGBA/luma LUT builder feeds a host-coherent
  Vulkan storage buffer; the NV12 shader applies independent RGB lookups followed by the same
  chroma-preserving luma scale as the CPU compositor before base grading. Non-NV12 graded hardware
  surfaces still fail safely back to the full CPU compositor.
- Completed: retain hardware frames whose decoded dimensions differ from the project output.
  `fittedPresentationRect` reproduces the CPU compositor's aspect-preserving, rounded fit and
  centering in output coordinates, while the two-rectangle presentation-quad overload keeps
  translations measured in project pixels rather than incorrectly scaling them by the fitted
  image width. Vulkan sampling performs the final display rescale without a CPU transfer.
- `build/jcut_imgui` and the neutral-boundary test must continue to pass
  `tests/check_imgui_binary_no_qt.py`. The final acceptance gate remains the exact unfiltered
  `./build.sh` command followed by the focused render/export tests.

## Implemented

### Neutral document and runtime

- Persistent media, track, and clip IDs.
- Media kind, source timing, clip timing, track assignment, and render ordering.
- Persisted audio-stream presence metadata for media-bin items and clips, with extension fallback
  for legacy unknown files.
- Track visual/audio flags, gain, mute, and solo state.
- Generated mask-child track identity (`generatedChildTrack`, `parentClipId`, and `childClipId`)
  round-trips through core/legacy JSON and both Qt/render bridge directions. A shared neutral
  reconciler creates one bounded child lane per valid matte, keeps it adjacent to its source,
  repairs stale/duplicate/orphan rows, recovers foreign ordinary clips instead of discarding them,
  and preserves the child lane's independent height, visual mode, and grading-preview state.
- Clip audio enablement, gain, pan, mute, and solo state.
- Transform, grading, opacity, and title state with keyframes.
- Effect, mask, and correction state.
- Clip ownership metadata (`clipRole` and `linkedSourceClipId`) round-trips through neutral JSON
  and the Qt/render bridges; unknown future roles remain opaque instead of being normalized away.
  Mask-matte children normalize their source/proxy, media, timing, playback-rate, and transform
  cache fields from the authoritative parent while retaining child-owned visual treatment.
- Transcript-overlay configuration.
- Render-sync markers and export ranges.
- Export request settings and transport state.
- Safe clip move, trim, single/razor-group split, delete, insert, selection, and nudge commands.
- Additive/toggle multi-selection plus reusable copy, cut, paste-at-playhead, duplicate, and
  select-all commands. Clipboard operations preserve relative track/frame layout and remap copied
  render-sync markers to fresh persistent clip IDs. Mask-matte ownership is treated as an
  aggregate: copying either a source or one of its children includes the source and all owned
  siblings, paste remaps their relationships, and destructive parent operations cascade without
  leaving orphaned children.
- Atomic selected-group delete, move, and nudge commands preserve relative clip layout, validate
  locked/out-of-range groups before mutation, and shift owned render-sync markers consistently.
  Parent move, group move, nudge, trim, resize, and playback-rate operations carry owned mask
  children as one timing aggregate and renormalize their caches; direct child timing edits are
  rejected instead of allowing parent/child drift.
- Clip lock/unlock is an undoable neutral command. Locked clips reject structural split, trim,
  move, resize, delete, and playback-rate edits at the runtime boundary; the shared playback-rate
  command applies the Qt duration/ripple policy and moves render-sync markers with rippled clips.
- Proxy discovery recognizes Qt-compatible `.proxy`, `.proxy.mp4`, and `.proxy.mov` siblings and
  validates either regular media files or image-sequence directories. ImGui can attach a proxy,
  discover a default proxy, enable/disable proxy playback, or clear the association through one
  undoable runtime command. Generated mask children inherit the authoritative source association;
  clearing an association never deletes the proxy file.
- Stable-ID track reordering preserves clip assignments and is independently undoable.
- Track-wide consecutive-clip crossfade state is represented neutrally by the Qt-compatible
  per-clip 48 kHz edge-fade length and round-trips through core/legacy JSON and both Qt/render
  bridge directions. The shared runtime command applies the Qt ordering, lock, duration,
  optional sample-domain overlap, and visual opacity-ramp policy as one undoable edit; generated
  child lanes cannot be targeted independently.
- Reusable media-lane conflict resolution routes timeline insertions without duplicating the Qt
  visual-versus-audio overlap policy in the shell. Generated child lanes are never returned as
  ordinary media targets, and indexed track creation supports the Qt drop-on-child-row behavior
  without treating the derived row as a reorder target.
- Atomic title-asset creation reuses that conflict policy and the existing neutral title/render
  path. It applies the Qt 90-frame visual-only defaults, creates a frame-zero title keyframe and
  stable persistent clip ID, selects the result, leaves the media bin untouched, always finds or
  creates the exact unnumbered `Titles` lane first, then reuses a free `Titles*` lane or creates
  the next numbered lane when the canonical lane is occupied.
- Channel-scoped grading, opacity, and transform keyframe removal is undoable; existing upsert
  commands are reused for Linear/Hold interpolation changes.
- Render-sync markers allow one mapping decision per canonical owner persistent clip ID and frame.
  Mask-child decisions canonicalize to the linked source, changing the operation updates that
  marker, counts follow the Qt `1..120` policy, and legacy duplicates load deterministically with
  the last value winning. Row edits and exact removal remain atomic and undoable instead of using
  broad clip/all-marker deletion.
- Atomic, undoable project-media removal rejects entries still referenced by timeline clips; it
  never cascades clip deletion. This matches the Qt shell's lack of a media-bin cascade policy.
- Undo/redo with deterministic snapshots and coalesced transactions for continuous controls and
  timeline drags.
- Redo invalidation after a new edit and separation of navigation-only state from edit history.

### Project and history interchange

- Loads the active Qt project through the same project root and active-project marker.
- Reads and writes the Qt `state.json` and `history.json` files.
- Preserves Qt-only nested clip/track fields while updating neutral fields.
- Preserves both legacy full-snapshot histories and snapshot-delta histories.
- Branches history at the active entry instead of retaining stale redo entries.
- Recovers malformed state from a valid snapshot-delta history.
- Rejects malformed or unsupported history before changing project files.
- Honors configured history entry and size limits while preserving the active snapshot.
- Rejects stale concurrent sessions instead of overwriting newer state.
- Serializes cross-process saves and uses atomic replacement for state/history commits.
- Rolls history back if the paired state commit fails.
- Validates a project before updating the active-project marker.
- Creates collision-safe normalized project directories, clones source history for Save As, and
  renames active projects without rewriting their state/history payloads.
- Coordinates managed-session saves with lifecycle mutations, rejects stale sessions after a
  rename, and rejects symlinked project paths that escape the active store.
- Activates and loads project switches as one shell operation, restoring the previous marker if
  the target cannot be loaded.
- Deterministic lifecycle failure injection covers New, Save As, Rename, and Activate marker
  commits, post-commit session-load rollback, rename rollback/recovery, and incomplete-project
  cleanup. Cleanup failures are retained safely and reported instead of being silently discarded.
- Neutral backend history listing reconstructs legacy and snapshot-delta entries with active index,
  playhead frame, and clip-count summaries. Atomic entry activation updates the paired state/history
  files without truncating redo entries, rejects stale or escaped sessions, and returns a refreshed
  managed session. The History tab lists those summaries and restores a selected saved snapshot
  behind the shared dirty-state guard. Saves, Save As, and Rename invalidate the shell's cached
  history rows so a newly adopted project can never act on indices displayed for the prior one.
- Persists ImGui media-bin entries through the legacy `mediaItems` extension. The Qt shell keeps
  that extension opaquely through project load, save, Save As, autosave, and history snapshots,
  so switching shells no longer discards the ImGui collection even though Qt still has no
  first-class media-bin UI for it.
- Canonical project parsing now gives the independently persisted image-sequence preference the
  same `jpeg` default in neutral and Qt legacy documents even when the active export is a video
  container. A shared-runtime command script opens both encodings, applies identical project,
  selection, grading, opacity, transform, track, sync, range, move, split, undo/redo, and title
  actions, compares the snapshot after every action, and compares the final Qt-compatible save.

### ImGui shell

- Real File/Edit/Playback menus and text-input-aware shortcuts for new project, save, Save As,
  reload, undo, redo, multi-select clipboard operations, split, delete, duplicate, nudge,
  play/pause, and frame stepping. New, Save As, and Rename reuse one lifecycle modal and shared
  project-store operations.
- Media browsing, import, guarded project-bin removal, track creation, clip insertion, and
  selection. Project-media removal is exposed through an item context menu and reuses the neutral
  runtime command.
- Timeline seek, additive/toggle select, single/group move, start/end trim, single/group razor
  split, boundary snapping with visible feedback, batch delete, group nudge, track reorder controls,
  and playhead behavior. The clip context menu exposes clipboard, nudge, split, playback-rate,
  lock/unlock, Scale-to-Fill, atomic grading reset, and counted render-sync actions through shared
  commands, and locked clips no longer present active drag/trim gestures. Colored render-sync
  markers are drawn on their canonical source clip, expose action/count tooltips, and open the same
  undoable actions when right-clicked.
- Project-bin and filesystem media drag/drop uses copied stable payloads, probed source durations,
  gap/empty-timeline track creation, and same-lane overlap routing. Track creation, reorder, and
  insertion are coalesced into one undo transaction. Numbered image-sequence directories reuse a
  Qt-free detector/natural frame listing across Qt, ImGui, probing, preview, and standalone export;
  they import as 30-fps visual clips whose duration is the detected frame count, while ordinary
  directories retain navigation behavior.
- Generated mask-child lanes have an indented/subdued timeline treatment and read-only derived
  identity. Ordinary media dropped on a child row creates a normal lane at that row boundary;
  ordinary clip moves and conflict routing reject child lanes. Matte clips do not advertise direct
  move/trim handles, and ordinary track reorder controls skip generated rows while source-track
  reorder/delete continues to carry the complete child hierarchy through the neutral runtime.
- Bound Grade, Opacity, Effects, Masks, Corrections, Titles, Sync, Transform, Transcript Overlay,
  Audio, Output, and Projects controls for the neutral state currently represented. Tracks expose
  neutral label/height editing, stable-ID reorder, tri-state visual mode, independent grading
  preview, and audio enable/gain/mute/solo through focused shared runtime commands. Generated rows
  keep selection, bounded height, visual mode, and grading preview editable while derived label,
  order, and audio controls remain unavailable.
- Tracks expose the Qt-style consecutive-clip crossfade duration and optional move-to-overlap
  controls through the neutral runtime command. Preview-audio timeline identity includes the
  per-clip fade, so a no-move crossfade cannot leave the reused Qt mixer on stale state.
- Effects consumes the shared Qt-free catalog of all 35 canonical preset IDs instead of keeping a
  partial shell-local list. The represented copies/radius, speed, scale/intensity, and alternate
  direction fields use preset-aware labels and ranges; neutral command normalization now matches
  the Qt `1..96` (`1..512` for progressive edge), `-8..8`, and `0.1..8` persistence bounds, so
  reverse and stationary effect motion survive ImGui edits. Speech-filter timing synchronization,
  difference-matte reference/threshold/softness, temporal-echo count/spacing/decay, and tiling
  pattern/spacing/wrap now also round-trip through neutral JSON and both Qt/render bridges and are
  directly editable in the ImGui Effects inspector.
- Mask-only controls use a focused neutral command, so toggling or tuning a mask does not normalize
  or overwrite unknown forward-compatible effect preset fields. The neutral document, JSON,
  runtime command, and Qt/render bridges now carry morphology, inversion/show-only, opacity,
  foreground/repeat layers, masked brightness/contrast/saturation, editable RGB/Luma curve points
  with smoothing, and drop-shadow enablement/radius/offset/opacity. The ImGui inspector binds all of
  those fields and reuses the shared grading-curve point editor.
- Transcript Overlay exposes every Qt-known placement and text-treatment field through the shared
  undoable runtime command: typography, opacity/colors, rounded background and padding, optional
  inset frame, configurable shadow, outline, stacked/eroded extrusion, speaker title, wrapping,
  paging, and word highlighting. These fields round-trip through neutral JSON and both Qt/render
  bridges rather than surviving only as opaque legacy data.
- Standalone preview/export now renders active transcript cuts through a shared Qt-free overlay
  layout core. It reuses the cut-session/catalog service, source/playback/render-sync mapping, and
  persisted export timing to select the active segment and word; UTF-8 FreeType/Fontconfig text,
  rounded/framed background, configurable shadow, outline/extrusion, speaker profile title,
  active-word colors, wrapping/paging, and normalized manual placement are composited after the
  visual tracks. When manual placement is disabled, a Qt-free speaker-profile evaluator follows
  persisted location/framing keyframes with the Qt smoothing curve. Audio-only clips are supported,
  and missing or invalid transcript cuts fail closed without disrupting the frame.
- Automatic speaker/face framing state is neutral, serialized, and bridged in both directions.
  The shared Qt-free framing core owns baked keyframe interpolation/retargeting and live face-box
  transform math for both Qt and standalone rendering. Standalone preview/export resolves the
  active transcript speaker, samples profile framing keyframes with confidence and box
  interpolation, maps media frames into the 30-fps transcript domain, and composes the result with
  the clip base transform. Direct hardware bypass is rejected whenever framing needs composition.
- Standalone framing now prefers an explicitly selected continuity track, then the active
  section/identity assignment from the shared transcript schema, before falling back to profile
  tracking. The shared `JCUTBOX1` reader samples assigned raw-track/stream geometry with linear
  interpolation and confidence filtering, so preview and export consume the same reviewed face
  assignments shown by the Speakers tab. Source-absolute, source-relative, and clip-timeline
  frame domains use the corresponding source-in/local-timeline lookup, while Qt-compatible
  typical-step bridge limits and configurable edge gap hold prevent stale tracks from leaking
  indefinitely. The standalone sampler and Qt continuity evaluator share the same robust
  confidence-weighted center/log-box smoothing implementation, and section rotation is preserved
  both with a sampled face box and as a clip-center fallback when no usable sample exists.
- The Transform inspector exposes automatic-framing enablement, baked target calibration,
  confidence, manual track/stream selection, center/zoom smoothing, mode/strength, gap hold, and
  enabled/baked/target keyframe creation, seeking, and removal through shared undoable commands.
- The Transcript tab now reuses a Qt-free cut-session/catalog service for source/audio-stream
  resolution, editable and numbered-cut discovery, custom labels, persisted catalog/custom-cut fallback,
  and unknown-field-preserving row projection. Its cached, clipped table shows timing, render
  ranges, speakers, text, edit flags, gaps, skipped words, and optional outside-cut rows without
  reparsing every frame. Cut selection is stored on the neutral clip so the rich render bridge and
  project history consume the same version, while the selected-cut legacy field remains mirrored
  for Qt compatibility. A Qt-free, unknown-field-preserving word mutation/save core now powers
  ImGui editing of text, raw source timing, skipped state, insertion, neighbor expansion, original
  restoration, render-order moves, and confirmed deletion on mutable cuts. Speaker/text filters,
  cut create/rename/confirmed-delete are bound. Transcript/profile mutations now enter the same
  globally ordered runtime undo/redo stack as timeline and project edits through transient,
  non-serialized document payloads. Undo/redo atomically restores the file and rolls the runtime
  navigation back if the file changed externally instead of overwriting it.
- The Qt `TranscriptTab` also delegates asynchronous read-only row construction, overlap handling,
  gaps, outside-cut projection, speaker labels, and edit flags to `TranscriptDocumentCore`. Its thin
  adapter maps projected active-word locations back to stable in-memory Qt word IDs, so refreshes
  and deletion do not retarget selection, follow state, or subsequent edits.
- The ImGui Speakers tab now projects a real roster from the same neutral transcript document:
  stored and word-only speaker identities, display name, organization, word count, and normalized
  title location are shown. Mutable cuts can edit name/organization/location through the shared
  unknown-field-preserving profile mutation and transcript history path.
- Contiguous speaker sections now have a shared Qt-free projection and inclusive range
  skip/unskip mutation used by both shells. The per-clip minimum-word threshold is neutral,
  undoable, serialized in core and legacy projects, and bridged in both directions. ImGui can
  expand a section table with speaker/range/word/snippet data, current section-track assignments
  and rotation diagnostics, seek to a section, and skip or unskip its words through global
  transcript history. A shared unknown-field-preserving section-options schema now owns rotation,
  grading, and mask settings. ImGui exposes those settings in a section modal and, when grading is
  enabled, creates the same section-start/end grading keyframes as Qt. Qt's existing Sections table
  and option save path delegate to the same projection/mutation rules instead of maintaining a
  second implementation. Per-section continuity-track assignment is also shared: ImGui can add
  its selected continuity tracks to a specific section or clear that section, while Qt's
  contiguous-section click assignment now calls the same neutral mutator. ImGui can export an
  individual section through shared source-FPS/playback-rate/render-sync-aware transcript-to-
  timeline mapping and the same persisted Output settings. Its bulk action coalesces adjacent
  same-speaker rows, derives deterministic track-aware filenames, skips existing/duplicate/
  unmapped outputs, and runs the remaining documents through the standard cancellable export
  worker with per-job progress and failure accounting.
- The ImGui Speakers tab can launch and cancel the existing
  `jcut_vulkan_facedetections_offscreen` SCRFD/Vulkan pipeline through a Qt-free reusable job
  controller. It uses the Qt media-sidecar directory convention, existing detector options,
  `jcut_processing_job_v1` manifest, generator log, resumable checkpoint, bounded
  terminate/kill cancellation, and required-output validation. Completion decodes the generator's
  Qt binary JSON envelope and concatenated qCompress/CBOR indexed-track records, merges unknown
  fields into the transcript-adjacent `JCUTBOX1` document, and refreshes the same neutral
  continuity inspector immediately. Detector/continuity algorithms remain in the shared existing
  executable rather than being copied into ImGui.
- Continuity-track references are now actionable in ImGui: a shared source-absolute/rate-aware
  mapper seeks the Program monitor to the selected track anchor, clamps navigation to the source
  clip, and overlays the stored normalized face box on the rendered frame. Stale selections are
  context-checked against the selected persistent clip before drawing.
- Qt and ImGui now share the same continuity-reference crop geometry. Up to eight selected ImGui
  tracks asynchronously decode their source anchors, apply the shared explicit-box or
  center/box-size crop policy, and compose a bounded reference strip alongside per-track frame,
  center, box, and confidence diagnostics. The strip composition is Qt-free and tested, while its
  Vulkan texture has independent ownership and is discarded when the selected tracks/source
  change, so reference decoding does not disturb Program presentation.
- Animated speaker introductions now use `speaker_title_core`, a Qt-free generator for
  transcript speaker-change detection, skipped-range-aware duration mapping, profile
  name/organization/logo/colors, all four 2D fly directions, and the 3D wrap animation. ImGui
  exposes style/duration/delay/fly-time/organization controls and commits generated
  `speaker_title` clips through one undoable replacement command that owns stable IDs and
  conflict-free `Speaker Titles*` lanes. The Qt fly-in adapter delegates animation/keyframe
  construction to the same core; the existing 53-case Qt effect-preset suite passes unchanged.
- Deterministic transcript mining now lives in `transcript_mining_core` and is reused by both
  shells. It proposes the same person-name and organization-suffix candidates as the prior Qt
  implementation, detects one-off/very-low-ratio word speaker labels, carries confidence and
  rationale, and applies only reviewed proposals while preserving unknown transcript fields.
  ImGui presents a selectable proposal table and commits selected name/organization/reassignment
  changes through global transcript undo. Qt name, organization, and cleanup actions delegate
  proposal/application behavior to the same core while retaining their native review dialogs.
- A Qt-free face-artifact adapter reads both `JCUTBOX1` JSON and Qt-compressed CBOR artifacts,
  including current/legacy face paths, continuity tracks, representative anchor geometry, frame
  coverage/domain, detector/run diagnostics, and identity cluster/assignment counts. ImGui can
  select continuity tracks, review their existing speaker ownership, assign them to the selected
  speaker with Qt-compatible fallback anchors, or clear that speaker's mappings; mutations retain
  unknown fields and participate in global transcript/project undo.
- Grade, Opacity, and Transform share a neutral-backed keyframe draft editor for every represented
  frame/value field, row load/seek, Linear/Hold changes, and scoped removal. Moving a keyframe
  coalesces remove-old plus upsert-new into one runtime undo step; legacy serialization carries
  future/other Qt-only material and mask fields to the reframed row when the key count is stable.
  All Qt-known tonal and curve grading fields are represented neutrally. The ImGui Grade draft
  binds all nine Lift/Gamma/Gain RGB values, and a shared full-grading evaluator supplies the
  interpolated or held grade, curves, and opacity used to seed New At Playhead. Grading reset
  returns all known fields to canonical defaults while preserving unknown future fields. A shared
  Qt-free grading-curve helper owns sanitization and three-point tone/curve mapping for both
  shells. ImGui exposes bounded RGB/Luma point tables, endpoint locking, add/remove/reset,
  smoothing, and three-point lock controls; locked tone edits regenerate the stored RGB curves.
  The same helper now owns Qt-compatible curve sampling, byte LUT construction, channel/Luma
  composition, deterministic bounded simplification, and Normalize Curves for both frontends;
  non-finite text/JSON input follows the prior Qt bounds instead of reaching an unsafe conversion.
  The ImGui curve canvas now matches Qt's click-to-add, constrained handle dragging, and
  right-click interior-point removal, including bidirectional three-point-lock updates. Auto
  Oppose uses one persistent standalone FFmpeg decoder per analysis run and the shared
  `editor_auto_oppose_core` RGBA statistics/event detector; Qt delegates to that same detector,
  while ImGui applies the resulting keyframes as one undoable history transaction.
- Title and track text editing uses Dear ImGui's dynamically sized `std::string` adapter, avoiding
  fixed-byte truncation and partial UTF-8 writes during otherwise unrelated style/property edits.
- The Titles tab creates a ready-to-edit title at the playhead through the atomic neutral command,
  using the same dedicated `Titles`/`Titles N` lane-routing policy as the Qt timeline action;
  command failures are surfaced through the shared shell status message.
- Keyframe frame cells seek through the neutral transport command. Sync rows support direct
  seeking, frame/count/action editing coalesced into one undo step, and individually undoable
  marker removal in addition to add/clear actions. The clip context menu reuses those commands at
  the playhead with Qt-matching `1..120` count bounds and same-action count defaults. Timeline
  markers are hit-testable and reuse the same context actions; popup and modal identity is guarded
  by both persistent clip ID and document generation across reload/project switches.
- Inspector controls use history transactions so one slider drag creates one undo step.
- Existing-project switching with validation, dirty-state protection, atomic marker/session
  adoption, and safe reload behavior.
- Export job status, progress, cancellation, and result reporting.
- Real histogram/scope input from CPU preview frames and live render/handoff telemetry.
- Movable and resizable panels after the initial layout, persisted in `imgui_layout.ini`.
- Persisted ImGui preferences and output-path drafts.
- Dirty-state protection for reload, project switching, and window close.
- Save/Discard/Cancel confirmation on a dirty window close.

### Preview and export hardening

- Preview work is asynchronous and the UI remains responsive while a frame is rendered.
- Borrowed zero-copy Vulkan frames are not reused until the UI acknowledges upload completion.
- Failed/stale previews clear their texture rather than displaying the previous frame as current.
- Control-server preview requests are transferred to the UI thread safely.
- Standalone FFmpeg export supports encoded MP4, MOV, and MKV video plus image-sequence output.
  Format selection explicitly controls the muxer even when the filename extension disagrees.
  WebM is routed correctly and uses portable libvpx VP8/VP9 encoders when the bundled FFmpeg build
  provides one; otherwise export fails clearly instead of selecting an unusable device encoder or
  silently writing MP4.
- The standalone FFmpeg boundary also performs import-time and legacy document-load stream probes,
  so silent visual media is not optimistically scheduled as audio. Stream presence remains
  independent of the clip's user-controlled Audio toggle.
- Export settings, playback speed, progress, cancellation, and output paths use the neutral render
  contract.
- Standalone encoded ImGui exports now contain audio. The Qt-free `standalone_audio_mixer` decodes
  embedded or external clip streams through FFmpeg into a canonical 48 kHz stereo cache, resolves
  relative project media paths, honors an explicit stream index with best-stream fallback, and
  treats advertised-but-undecodable audio as an export error instead of silently writing a
  video-only file.
- The neutral mixer owns timeline/source mapping, render-sync duplicate/skip adjustment,
  clip/track enablement, gain/pan/mute/solo policy, clipping, and per-clip edge fades. The export
  backend creates AAC (MP4/MOV/MKV) or Opus/Vorbis (WebM) streams before the container header,
  converts the mixed float stream to the encoder format, and interleaves encoded packets with the
  video container. Synthetic policy coverage and a real WAV-to-MP4 export test verify both the
  mixer and the presence of encoded audio packets.
- The Rubber Band offline algorithm now lives in `jcut_audio_time_stretch_core`, a Qt-free service
  reused by the existing Qt wrapper and standalone export. Non-unit clip playback rates transform
  their decoded cache without changing pitch, and non-unit export playback speed transforms the
  completed mix to the video duration. A frequency-domain regression fixture verifies that a 2x
  stretch retains a 440 Hz source tone, and the encoded export test verifies the shortened stream
  duration. Long-source decoding and non-unit whole-mix stretching should still move from
  whole-buffer operation to bounded/ranged processing as a scalability hardening step.

### Neutral preview/audio boundaries

- `OffscreenVulkanFrame` now lives in a small Vulkan/core header rather than the Qt-heavy render
  internals header.
- `ImGuiAudioRuntime` is a Qt-free PIMPL implementation used by the shell. It drives RtAudio from
  the standalone decoder/mixer cache and maps neutral transport position and speed directly.
- Audio timeline changes are installed as one mixer-visible state update, explicit/relative audio
  sources are resolved before conversion, and visual-only edits do not change the audio timeline
  signature or trigger reconstruction; throttled filesystem identity polling continues
  independently.
- Audio warmup runs asynchronously, exposes an explicit buffering state, and holds the neutral
  transport while decode/time-stretch work is still pending. A timed-out warm interval is retried
  without advancing transport; playback starts only after audio is ready, after a confirmed
  terminal source failure (allowing the rest of the timeline to continue), or when the output is
  unavailable and the neutral transport must provide the clock.
- Filesystem identity checks are throttled to 250 ms and cover source files, explicit audio files,
  and derived same-stem WAV sidecars using existence, size, and modification time. Changes
  invalidate versioned decode/time-stretch caches and refresh stream topology when media at a
  stable path is replaced. Stale decode, RubberBand progress/failure, sidecar-write, and mixer work
  cannot repopulate the new generation or refill cleared buffers.
- Ring-buffer reset now rendezvouses safely with the real-time reader, avoiding a seek/project-
  switch race that could otherwise expose overwritten samples.
- `FramePayloadCore` owns neutral frame identity, dimensions, crop metadata, opaque CPU/GPU
  payload bookkeeping, AVFrame clone/free lifetime, and the existing CPU/GPU residency estimates.
  `FrameHandle` remains the Qt adapter seen by current consumers, but no longer dereferences QRhi
  private types in the ImGui chain. Its production-unused QRhi texture factory/upload methods stay
  source-compatible in a Qt editor/test-only adapter, preserving the legacy Qt API while keeping
  private QRhi out of the ImGui targets. This eliminates `Qt6::GuiPrivate` from
  `jcut_preview_decode_runtime` and `jcut_runtime_support` without changing decode, cache, rich
  preview, or audio behavior.
- `VulkanFrameImporter` is a Qt-free PIMPL interface used by the shell and now owns the shared
  Qt-free opaque-FD import core directly. `VulkanDetectorFrameHandoff` delegates its external-frame
  import/copy path to that same core, preserving the existing local sampled-image copy, layout
  restoration, borrowed-frame lifetime, and caller-owned ready-semaphore behavior without
  duplicating the implementation. The shell's `PreviewRenderResult.image` path retains CPU
  fallback.
- `jcut_preview_decode_runtime` is an AUTOMOC object target containing the existing
  `AsyncDecoder`, `MemoryBudget`, and `TimelineCache` implementation cluster. Its generated MOC
  object is separate from the `ProjectManager` and `TranscriptTab` MOCs that remain in
  `editor_test_support`, so using preview decode/cache no longer pulls Qt widget implementations
  into `jcut_imgui`.
- `jcut_runtime_support` contains the non-widget legacy helpers still needed by preview and audio.
  Widget dialogs, project UI, and transcript-tab implementations remain in
  `editor_test_support`; the ImGui CMake chain no longer names Qt Widgets, Qt Network, Qt
  Concurrent, or Qt GuiPrivate, and a generated link-interface manifest test guards that
  boundary. Image-sequence
  batch loading retains bounded parallelism through a Qt Core thread pool instead of
  `QtConcurrent::blockingMapped`; fire-and-forget waveform/continuity jobs use the same Core pool,
  while the future-based transcript save controller remains with widget-facing test/editor
  support.
- A standalone neutral-boundary executable includes the public editor, preview, audio, and frame-
  import contracts without instantiating platform backends; it links only `jcut_editor_core`
  and Vulkan and passes the strict no-Qt linkage checker.
- A focused importer-boundary executable instantiates the real `VulkanFrameImporter`, exercises
  inert/invalid lifecycle behavior, and is guarded by the strict no-Qt linkage checker.
- The linkage checker accepts a focused Qt component, and the final executable passes the focused
  no-Widgets, no-Network, and no-Concurrent checks.

## Current Target Boundaries

| Target | Qt-free | Responsibility |
| --- | --- | --- |
| `jcut_frame_payload_core` | Yes | Neutral decoded-frame identity, size/crop metadata, opaque payload ownership/bookkeeping, AVFrame lifetime, and memory accounting. |
| `jcut_frame_handle_qrhi_adapter` | No | Legacy `FrameHandle` QRhi factory/upload compatibility, linked only by Qt editor/test targets and excluded from the ImGui runtime chain. |
| `jcut_transcript_document_core` | Yes | Unknown-field-preserving WhisperX parsing/projection/mutation, cut catalog and atomic saving, speaker profiles/assignments, and `JCUTBOX1` JSON/compressed-CBOR face/identity artifact inspection reused by ImGui and Qt-facing adapters. |
| `jcut_media_path_core` | Yes | Shared image-sequence-directory detection, naturally ordered frame listing, and proxy sidecar discovery/usability policy. |
| `jcut_decoder_policy_core` | Yes | Shared decode-preference and hardware-device parsing, hardware/zero-copy fallback, H.264/H.265 software-thread normalization, deterministic mode, and decoder-lane preference storage. |
| `jcut_ffmpeg_hardware_device_core` | Yes | Shared FFmpeg hardware-device ordering/context creation and hardware pixel-format selection reused by the Qt format adapter and standalone ImGui decoder. |
| `jcut_editor_core` | Yes | Neutral document JSON, project I/O, runtime commands/history, render-contract JSON, and control server. |
| `jcut_vulkan_frame_import_core` | Yes | Shared opaque-FD external-memory import, local sampled-image copy, layout restoration, and Vulkan resource ownership. |
| `jcut_imgui_vulkan_import` | Yes | Dear ImGui's neutral PIMPL facade over the shared Vulkan import core. |
| `jcut_audio_time_stretch_core` | Yes | Shared Rubber Band offline pitch-preserving conversion reused by the Qt wrapper and standalone ImGui audio/export. |
| `jcut_imgui_standalone_runtime` | Yes | Standalone timeline/image-sequence rendering, audio decode/mix, import/load media stream probing, and FFmpeg video/audio export. |
| `jcut_preview_decode_runtime` | No | Existing QObject-based asynchronous decoder, memory budget, and timeline cache, isolated in its own AUTOMOC object target. |
| `jcut_runtime_support` | No | Non-widget legacy renderer/decoder, audio, transcript, and timing helpers; exposes Qt Gui/Core but not Widgets, Network, or Concurrent. |
| `editor_test_support` | No | Widget-facing dialogs, project UI, and transcript-tab support retained for the Qt editor and tests; excluded from the ImGui dependency chain. |
| `jcut_editor_runtime` | No | Existing Qt-backed render bridge, Vulkan renderer, overlays, titles, and audio engine. |
| `jcut_imgui_audio_runtime` | Yes | RtAudio output, asynchronous standalone decoding, filesystem/sidecar refresh, neutral transport synchronization, and standalone mixing. |
| `jcut_imgui_runtime` | Yes | Qt-free standalone CPU preview facade plus audio and Vulkan-import services. |
| `jcut_imgui` | Yes | X11/Vulkan/ImGui shell; the strict final-binary check reports no Qt linkage. |
| `editor` (`jcut`) | No | Existing Qt frontend, kept working throughout migration. |

The current dependency chain is:

```text
jcut_imgui
  -> jcut_imgui_runtime
     -> jcut_imgui_standalone_runtime
        -> jcut_editor_core + jcut_media_path_core + jcut_decoder_policy_core
           + jcut_ffmpeg_hardware_device_core + jcut_audio_time_stretch_core
     -> jcut_imgui_vulkan_import -> jcut_vulkan_frame_import_core
     -> jcut_imgui_audio_runtime -> RtAudio + jcut_imgui_standalone_runtime
```

The final Linux ImGui binary and its generated target interfaces are now Qt-free. The strict
all-Qt binary check, focused component checks, and CMake-interface regression check guard that
boundary. Qt remains in the separate legacy editor/runtime targets shown above for the Qt shell.

## UI Parity Matrix

| Area | Current ImGui coverage | Work still required for Qt parity |
| --- | --- | --- |
| Projects | Safe new/save/Save As/rename/load/reload and existing-project switching, with dirty-state guards, shared-store collision handling, stale-session/path validation, and transactional failure-injection coverage | Qt-native dialog polish and exact cross-shell UX equivalence |
| Project history | Preserved disk history, runtime undo/redo/depth, and a saved-snapshot browser with atomic restore and redo preservation | Rich history metadata and cross-shell command equivalence |
| Media | Browse, filter, import, safe project-bin removal, cross-shell preservation, add track, insert clip, project/filesystem drag/drop, naturally ordered image-sequence-directory import/drop/render, Qt-equivalent filesystem context actions to open a folder/containing folder and copy an absolute path, asynchronous hover/selection thumbnails decoded through the shared Qt-free media decoder and presented on an independently owned Vulkan texture, undoable proxy discover/attach/use/disable/clear association, cancellable shared-renderer JPEG/H.264/MJPEG generation, JPEG continuation, explicit overwrite consent, and confirmed path-constrained managed deletion | Exact Qt tree/gallery presentation and native dialog polish |
| Transport | Seek, play/pause, start/end, frame step, and Qt's exact 10/25/50/75/100/125/150/200/300% presets are bound. Qt and neutral runtime share export-range-aware forward/back stepping; neutral ticking skips disjoint range gaps, loops to the first playable range, stops at the final playable frame, and restarts from a valid range. Loop, video/audio view mode, master mute, and 0–100% volume are durable across neutral and legacy project JSON; ImGui's RtAudio callback applies mute/volume without rebuilding the timeline and publishes a lock-free post-gain live waveform for Audio view | Final native styling |
| Timeline | Additive/toggle selection, parent-authoritative source/mask timing normalization, rejection of direct child timing edits, relationship-safe aggregate move/nudge/trim/resize/rate/split/copy/delete with marker movement, generated-child reconciliation/presentation/drop protection, visible snapping, batch delete/nudge, copy/cut/paste, duplicate, clip rate/lock, Scale-to-Fill, Qt-complete grading reset, visible/right-clickable counted render-sync markers, stable-ID source-track reorder with child grouping, media drag/drop with gap creation/conflict routing, export ranges, and core context actions. Context actions also copy clip/title text, perform undoable selected-clip source metadata/FPS/audio/duration refresh with Qt full-source-duration preservation, launch real WhisperX transcription, route BiRefNet/SAM3 into their functional Masks workflows, and reach grading, sync, speaker/face, proxy, Jobs, and properties | Group-drag and exact native menu styling polish |
| Tracks | Neutral label/height editing, stable-ID reorder, tri-state visual mode, independent grading preview, audio enable/gain/mute/solo, and consecutive-clip crossfade duration/overlap controls are bound; generated rows preserve editable visual/grade/height state while derived identity/order/audio remain guarded. A shared legacy-compatible media-presence policy gives Qt and ImGui the same visual/audio capability decisions, and ImGui disables unavailable controls with an explicit reason. Timeline-row `V`/`A` affordances reproduce Qt's Enabled → Force Opaque → Hidden cycle and audio toggle without turning disabled or derived rows into mutable controls | Final native styling polish |
| Preview | Qt-free asynchronous standalone preview with zoom/pan, source-in/playback-rate/render-sync frame mapping, ordered multi-track alpha composition, hidden/force-opaque track policy, neutral animated grading/opacity/transform evaluation, baked/profile/manual/section/identity-assigned automatic speaker/face framing through shared Qt math and the shared `JCUTBOX1` adapter, explicit continuity frame-domain mapping, gap hold, shared robust center/zoom smoothing, section-rotation fallback, and transcript-derived speaker-mask palettes, FreeType/Fontconfig advanced title rasterization, active-cut transcript overlays, fail-closed advanced masks, and standalone implementations for all 35 canonical effect presets. FFmpeg hardware decode is functional with capability-aware CPU transfer and runtime software fallback; the CUDA-to-Vulkan direct path keeps the lowest eligible video transformed, opacity-keyed or force-opaque, automatically speaker-framed, and fully tonal/RGB/luma-curve graded on GPU. Every upper visual track—including decoded videos, images, titles, effects, masks, and corrections—uses the same renderer on a transparent canvas and merges with subtitles into one alpha-bounds-cropped Vulkan overlay | Move base-layer effects/masks/corrections and upper-layer/title/subtitle rasterization onto the GPU, add non-CUDA hardware import, and reach exact GPU/CPU rich-composition parity without relinking the Qt renderer |
| Grade | Base brightness/contrast/saturation and keyframe values use Qt's full `-10…10` domain; frame/opacity/interpolation and all nine Lift/Gamma/Gain RGB values are editable. Row load/seek, creation/removal, neutral full-grade evaluation for New At Playhead, direct click/add/drag/right-click-remove RGB/Luma curve canvases plus bounded numeric point editing, smoothing/three-point lock with shared bidirectional tone synchronization, shared Qt-compatible curve normalization, neutral storage of all Qt-known curve fields, atomic Qt-complete grade/opacity reset, and asynchronous decoded-source Auto Oppose with the same settings/detection core as Qt are bound | Final native styling polish |
| Opacity | Value, fades, and full neutral keyframe frame/value/interpolation editing with row load/seek, creation, and removal are bound | Direct preview manipulation and exact Qt workflow polish |
| Effects and masks | All 35 canonical presets and the Qt-known speech-sync, difference/echo, and tiling parameters are neutral, serialized, bridged, bound, and rendered by standalone preview/export. Speaker-mask colors resolve from the active transcript profile with Qt-compatible fallbacks. A shared Qt-free sidecar core gives Qt and ImGui the same stable identity, discovery, ordinal-map/source-identity validation, readiness, and fail-closed policy. Qt and ImGui share SAM3 and BiRefNet job plans. ImGui exposes SAM3 cache/performance/output controls and BiRefNet model/revision/cache/device/FP16/tolerance controls, with resume/restart/cancel, manifest/progress/log inspection, a 250-ms independently textured live preview, validated output, and atomic generated-child materialization with Qt ownership defaults. Treatment and automatic/explicit Z order remain child-owned | Exact GPU/CPU sampling/transform parity and final native-dialog polish |
| Corrections | Global enablement plus undoable polygon creation, selection, enable/range editing, individual/all deletion, normalized vertex-table editing, and direct Program-monitor vertex dragging are bound. Drawing mode supports click-to-draft, live overlay, close, and cancel; active/inactive persisted polygons are visibly distinguished and standalone preview/export applies them | Exact Qt tool-mode/focus gestures and final overlay interaction polish |
| Titles | All Qt-known title-keyframe fields are neutral, serialized, bridged, editable, and evaluated: text/font/placement/emphasis/opacity, auto-fit, logo, solid/neon/stripe/grid/image materials, text/frame patterns, shadow, window/frame, stacked/eroded extrusion, and 3D orientation/depth/scale. Standalone preview/export renders these treatments and decodes referenced pattern/logo assets; window, patterned frame, logo, shadow/extrusion, and glyphs all participate in the evaluated 3D transform. `title_3d_projection_core.h` reproduces Qt's Vulkan camera, perspective, rotation order, per-element layout, depth bounds, and scale bounds, with direct `QMatrix4x4` corner parity. Atomic creation, row load/seek/removal, and direct output-space dragging with coalesced undo are bound | Exact Vulkan projective texture sampling/occlusion and extrusion mesh-shader equivalence, plus final handle/selection interaction polish |
| Sync and transform | Sync add/clear/individual removal, row seeking, frame/count/action editing, canonical source ownership, and visible tooltip/right-click timeline markers are bound. Transform exposes full neutral frame/title/translation/rotation/scale/interpolation editing with row load/seek, creation/removal, Scale-to-Fill, undoable source-transform locking with cycle-safe standalone evaluation, and direct Program-monitor move/horizontal/vertical/uniform resize/rotation handles. Preview drags reuse shared anchored-resize and wrap-safe pointer-angle math, support Shift 15-degree snapping, and commit base-relative temporal keyframes with coalesced undo. The complete durable speaker-framing state is neutral, rendered, and authorable through scalar plus enabled/baked/target keyframe controls | Add source-geometry-perfect hit bounds for every generated effect and exact Qt interaction polish |
| Transcript | Every neutral-backed overlay field is bound; ImGui uses the Qt-free cut-session/catalog core for cached path/version loading and a clipped editable table with gaps, edit flags, speaker labels, skipped/outside-cut rows, persisted cut selection, filters, and rich-preview propagation. Mutable cuts support Qt-compatible text/raw timing/skip, insert, expand, restore, reorder, confirmed word deletion, and cut lifecycle. Transcript/profile mutations share the globally ordered runtime undo/redo stack with project edits via transient non-serialized payloads; file restoration is atomic and external changes fail closed with runtime navigation rollback. Qt and ImGui now share word patch, batch skip/delete, insertion, expansion, original restore, arbitrary render reorder, and create/rename/delete cut services; Qt preserves unknown fields and stable projected word identity across structural mutations | Final native table/dialog styling polish |
| Speakers/faces | ImGui shows a neutral transcript-derived roster with profile and word-only identities, editable name/organization/title location, and word counts. Qt and ImGui share contiguous-section projection and range skip/unskip; ImGui exposes the persisted minimum-word threshold, section rows, assignment/rotation diagnostics, seek, and transcript-history-backed edits. Section rotation, grading, mask options, and additive/clear continuity-track assignment use the same unknown-field-preserving schema in both shells; ImGui edits them directly and emits Qt-equivalent start/end grading keyframes. Individual and bulk section export map transcript frames through source FPS, trim, playback rate, and render-sync state into exact neutral export ranges; bulk jobs coalesce/name/skip like Qt and share the cancellable exporter. Assigned continuity tracks render as bounded asynchronous face-avatar strips directly in section rows, using the same neutral crop geometry as Qt without blocking playback or the UI. The shared artifact adapter reads Qt `JCUTBOX1` JSON/compressed-CBOR continuity and identity artifacts; ImGui reviews track coverage/geometry/detector diagnostics and existing owners, assigns/clears selected tracks with Qt-compatible anchors through global undo, seeks to a selected reference anchor, outlines its face box in Program, and asynchronously displays a bounded multi-selection crop strip using the same explicit-box/center-size geometry as Qt. ImGui launches/cancels the existing offscreen SCRFD/Vulkan generator with Qt-compatible sidecar/manifest/resume contracts and imports its Qt binary/indexed output into the shared transcript artifact. Its preflight exposes stride/threshold/topology/primary/small-face/tiling/zero-copy compatibility plus optional generator control window, live preview window, explicit checkpoint-clearing restart, source/proxy input selection while keeping sidecars anchored to the source clip, and selected-clip grading through the shared Qt-compatible clip JSON projection. The shared job core can run the generator's real pipeline benchmark before launch, persist `launch_control.json`, and reload/apply its saved worker/slot recommendation. Shared transcript-driven animated introductions produce durable, undoable `speaker_title` clips with all Qt fly-in styles. Shared reviewed local/cloud mining proposes/applies names, organizations, and spurious-label cleanup. The Qt generator currently scans one contiguous clip source range; its help text's mention of dialogue-only scanning is stale and is not treated as a Qt-parity requirement | Deeper identity-cluster decisions and remaining diagnostics |
| Audio | Qt-free RtAudio preview/status plus standalone decode/mix; clip/track gain/pan/mute/solo/fades, sync mapping, sidecar refresh, async warmup, persisted output-device discovery/selection with safe stream restart, pitch-preserving clip/export speed, shared two-stage Harmonic Speech Isolation, transcript-aware per-word normalization, and shared persisted master amplification/normalization/peak-reduction/compressor/soft-clip/limiter/stereo-to-mono DSP in preview and export. Qt and neutral projects preserve clip/track `audioBusId`; the current Qt shell has no bus graph, mixer behavior, or routing control to reproduce | Define and implement a real shared bus-routing model in both shells, advanced backend/device capabilities, and streamed long-source processing |
| Export | MP4/MOV/MKV video+audio or image sequence, explicit muxer routing, pitch-preserving speed, WebM selection with clear portable-codec requirements, settings, progress, cancel, and exact bounded/discontiguous range concatenation for video, image numbering, progress segments, and audio. Individual and queued bulk speaker-section exports use the same range-aware worker | Bundle/require portable VPx and Opus/Vorbis encoders for universal WebM output; bounded audio processing and remaining option/output-equivalence coverage |
| Jobs | Export, face detection, proxy generation, SAM3 prompt masks, WhisperX transcription, and BiRefNet alpha generation are functional. Jobs reports independent state/progress/cancellation, supports interactive transcription stdin, exposes output/manifest/status diagnostics, and provides bounded refreshable inspection of manifests, progress, and worker logs. Masks renders the evolving BiRefNet preview on an independent texture | Other general processing jobs and domain-specific result viewers |
| Scopes/pipeline | Live normalized luma/R/G/B preview histograms exceed the Qt grading histogram's channel coverage. Pipeline now presents a selectable Timeline Map → Decode → GPU Import → Composite → Present graph with live state/exactness, per-stage facts, source/device/fallback diagnostics, explicit refresh, and zero-copy retry | Per-stage image thumbnails and final native graph styling |
| AI/access | Reviewed deterministic speaker name/organization/spurious-label cleanup is functional in Speakers. A shared Qt-free libcurl gateway core gives ImGui functional asynchronous entitlement/usage/license refresh, populated access rows, model selection, budget/rate enforcement, ordered model fallback/retry, Supabase-direct fallback, stateful project-context chat, cancellable browser-PKCE login, refresh-token exchange, subscription checkout launch, and review-first cloud speaker name/organization proposals without persisting bearer tokens in projects. Qt and ImGui share the same credential implementation for Linux secret attributes and the private config fallback; Qt reuses the same gateway normalization and token-profile parser. ImGui presents the authenticated identity with basic/enabled/subscribed status, bounded time/phase/summary activity, and a bounded asynchronous remote avatar with rounded Vulkan presentation plus initials fallback | Exact native presentation polish |
| Preferences/system | Persisted layout/font and panel defaults; Qt-compatible project autosave/history retention with atomic bounded backups; configurable RtAudio buffer size and persisted output-device selection with stream restart and requested/actual diagnostics; shared decode preference, explicit persisted hardware-device selection, H.264/H.265 threading, deterministic mode, lane preference, functional preview-decoder refresh, requested/effective hardware/software and device diagnostics, runtime fallback reasons, and an asynchronous source decode benchmark | Direct zero-copy decoded-frame composition, decoder prefetch/cache controls, startup/profile benchmark automation, applying lane count to concurrent decode scheduling, and remaining persistence |

## Remaining Implementation Order

1. **Remove Qt from the ImGui preview/audio dependency chain.**
   - Completed: move the neutral Vulkan frame type out of `render_internal.h`.
   - Completed: make the ImGui shell consume neutral audio and external-frame import interfaces.
   - Completed: preserve the existing `AudioEngine` behind its neutral adapter and preserve the
     zero-copy importer behind a neutral facade while extraction continues.
   - Completed: isolate preview decode/cache QObjects and their MOCs in
     `jcut_preview_decode_runtime`; the final executable now passes the focused no-Qt-Widgets
     linkage check.
   - Completed: split non-widget runtime helpers into `jcut_runtime_support` and keep widget-facing
     test/editor support out of the ImGui chain, so its CMake link interface exposes neither Qt
     Widgets nor Qt Network.
   - Completed: replace image-sequence `QtConcurrent::blockingMapped` with bounded Qt Core pool
     workers and remove Qt Concurrent from the ImGui binary and target interfaces.
   - Completed: extract opaque-FD external-frame import/copy into
     `jcut_vulkan_frame_import_core`, make both the ImGui facade and legacy handoff reuse it, and
     split the importer out of the former `jcut_imgui_qt_adapters` target. This was the first
     external-frame boundary reduction before the audio and preview targets became Qt-free.
   - Completed: extract decoded-frame ownership, normalized crop metadata, and residency
     accounting into `jcut_frame_payload_core`; retain `FrameHandle` as a Qt adapter and remove
     its unused private-QRhi creation/upload implementation from the ImGui target chain (while
     retaining it for Qt editor/test compatibility), so the ImGui runtime targets no longer expose
     Qt GuiPrivate.
   - Completed: replace the ImGui `AudioEngine` adapter with RtAudio plus standalone decode/mix and
     the shared Qt-free Rubber Band core; retain async warmup, source/sidecar refresh, status, and
     transport synchronization.
   - Completed: make `jcut_imgui_runtime` depend only on Qt-free targets and pass the strict final
     `tests/check_imgui_binary_no_qt.py` check (including removal of Qt6 Gui/Core/DBus).
   - Completed: add ordered multi-track alpha composition, hidden/force-opaque track policy, and
     shared neutral animated grading/opacity/transform evaluation (including tone and curve LUTs,
     translation, rotation, scale, and base/keyframe composition) to the Qt-free standalone
     preview/export renderer.
   - Completed: map standalone video decode through source-in, clip playback rate, source-duration
     clamping, and render-sync duplicate/skip markers instead of decoding raw local timeline frames.
   - Completed: render ordinary animated title clips in preview and export through a shared neutral
     Linear/Hold evaluator and Qt-free Fontconfig/FreeType raster path, including UTF-8, multiline
     layout, font family/style, color, placement, size, opacity, and clip grade/transform composition.
   - Completed: promote every remaining Qt-known title-keyframe field through neutral JSON and both
     Qt/render bridges, bind the fields in the ImGui inspector, and render auto-fit, logos,
     procedural and decoded-image materials, shadows, windows/frames, stacked/eroded extrusion,
     and 3D orientation/depth/scale in standalone preview/export. The CPU raster treatment remains
     an approximation; exact Qt Vulkan projective sampling and mesh output remain exit blockers.
   - Completed: apply the standalone title 3D transform to the complete composited title layer,
     including window, patterned frame, and logo rather than transforming only glyph/shadow/
     extrusion pixels; the render fixture proves high-yaw window geometry differs from the
     axis-aligned result.
   - Completed: replace the standalone cosine-scale title approximation with a Qt-free point
     projection that matches Qt's 43-degree camera, 5.2 camera distance, yaw/pitch/roll order,
     per-element unscaled layout offsets, scaled quads, perspective depth, and Qt bounds. A direct
     four-corner `QMatrix4x4` comparison proves sub-pixel parity under combined transforms.
   - Completed: extract strict mask-frame resolution into a Qt-free service reused by Qt and ImGui.
     Generated ordinal/VFR sidecars validate map ordering/hash, metadata, source identity,
     completion manifests, and exact frame coverage before rendering; stale, incomplete, missing,
     or out-of-map mattes fail closed. Standalone preview/export applies mask alpha/show-only,
     inversion, erosion/dilation, feather/blur shaping, opacity, active correction polygons,
     masked grading with RGB/Luma curves, foreground layering, centered repeat copies, and drop
     shadows. Those advanced fields now round-trip through neutral JSON and both Qt/render bridges,
     and the ImGui Masks inspector reuses the shared curve-point editor for direct editing.
   - Completed: render persisted active transcript cuts for preview/export through a shared Qt-free
     layout core and the existing catalog/session projection. The standalone compositor covers
     source timing, padding/offset edge guards, wrapping/paging, active-word selection, UTF-8
     typography, rounded/framed background, configurable shadow, outline/extrusion, speaker title,
     highlight colors, normalized manual placement, and automatic active-speaker placement from
     transcript framing metadata, including audio-only timelines; a real JSON-backed render
     fixture guards the behavior.
   - Completed: promote the complete durable speaker-framing contract into the neutral document,
     JSON, and Qt/render bridges; share baked interpolation/retargeting and face-box transform math
     between Qt and standalone; apply baked and transcript-profile dynamic framing in standalone
     preview/export with confidence and transcript-frame mapping.
   - Completed: bind every neutral framing scalar and enabled/baked/target keyframe authoring,
     seeking, and scoped removal action in the ImGui Transform inspector through shared commands.
   - Completed: resolve manual, active-section, and identity continuity assignments through the
     shared transcript/`JCUTBOX1` core and apply confidence-filtered interpolated face samples in
     standalone preview/export, with profile tracking as the fallback.
   - Completed: map source-absolute, source-relative, and clip-timeline continuity domains and
     enforce Qt-compatible typical-step bridge/edge-hold behavior plus the persisted gap-hold
     override.
   - Completed: extract robust confidence-weighted center/log-box smoothing into a Qt-free core
     used by Qt and standalone continuity evaluation, and preserve active-section rotation both
     around sampled face boxes and through the no-sample clip-center fallback.
   - Completed: project primary/secondary/accent colors through the neutral speaker profile and
     apply the active transcript speaker's palette to standalone speaker-mask effects with Qt's
     red/green/yellow fallbacks.
   - Restore remaining rich preview composition and zero-copy Vulkan production behind the
     Qt-free preview interface. Exact Vulkan
     title/composition output and GPU production remain render-equivalence blockers.

2. **Adopt the shared runtime from the Qt shell.**
   - Route equivalent Qt editing actions through `EditorRuntime` commands.
   - Completed: add a command-script regression that starts from both the neutral ImGui document
     and Qt's legacy state encoding, requires identical canonical snapshots after every shared
     action, and requires byte-equivalent normalized Qt save payloads. The test exposed and fixed
     an empty-versus-`jpeg` image-sequence preference default that previously dirtied a project
     merely by switching shells.
   - Completed: extract export-range normalization plus set-start/set-end/split/reset rules into
     `export_range_core.h`; both `EditorRuntime` and Qt `TimelineWidget` now delegate to it, and a
     direct Qt-versus-neutral edit-sequence test guards normalized ranges and split rejection.

3. **Finish high-frequency editing workflows.**
   - Completed: neutral multi-selection and clipboard commands, atomic group move/delete/nudge,
     stable-ID track reorder, shell shortcuts/Edit menu, and core timeline context actions.
   - Completed: shared razor-group splitting, visible move/trim/drop snapping, and project/filesystem
     media drag/drop with empty/gap track creation and same-lane conflict routing.
   - Completed: shared image-sequence-directory detection, natural frame ordering, deterministic
     30-fps/frame-count probing, filesystem/project insertion, and standalone frame decoding.
   - Completed: neutral clip lock enforcement plus Qt-style playback-rate duration/ripple behavior,
     bound with nudge, rate, and lock/unlock actions in the ImGui clip context menu.
   - Completed: reuse the undoable neutral transform command for Scale to Fill Preview, share its
     aspect-fit multiplier with the Qt shell, restore Qt still-image size probing, and expose
     Qt-free still/video/image-sequence source dimensions through the standalone FFmpeg probe.
   - Completed: reuse neutral render-sync commands from the clip context menu with Qt count/default
     semantics, draw and hit-test canonical-owner timeline markers with the same context commands,
     and guard their modal/context identity across document reloads.
   - Completed: model every Qt-known grading tonal/curve field and add an atomic grading reset that
     restores those fields, neutral base grade/opacity, and frame-zero visual keys without
     disturbing unrelated or unknown future clip state.
   - Completed: preserve source/mask-matte ownership through parent-authoritative cache
     normalization, rejection of direct child timing edits, aggregate move/nudge/trim/resize/rate,
     copy/paste/split/delete, and relationship/render-sync marker remapping.
   - Completed: neutral generated-child track identity/reconciliation, JSON and Qt/render bridge
     round trips, derived-row presentation, indexed normal-lane drops, guarded child targets,
     source-track hierarchy reorder/delete, and independently editable child visual/grade/height
     state.
   - Completed: neutral consecutive-track crossfade command, per-clip fade persistence/bridging,
     ImGui duration/overlap controls, generated-lane guards, and preview-audio signature refresh.
   - Completed: make multi-selection drag snapping treat the selection as one rigid aggregate:
     every selected start/end edge can snap to an external clip boundary, internal and generated
     mask-child edges are excluded, the earliest selected clip cannot cross frame zero, and the
     existing atomic move command/history transaction remains authoritative.
   - Completed: preserve multi-selection in the clip context menu for atomic split-at-playhead and
     lock/unlock actions, with the new group lock command sharing runtime undo/redo and ignoring
     derived mask children.
   - Completed: make `Ctrl+B`, the Edit menu, and the clip context menu share one selected-group
     split eligibility policy and the atomic split command; global timeline shortcuts remain
     suppressed while an editable/active ImGui item owns input, matching Qt's editable-focus guard.
   - Completed: reuse a neutral undoable export-range context command for Qt-compatible
     set-start, set-end, split, and reset semantics, including discontiguous ranges and synchronized
     render-request bounds/count. The timeline context menu is now available over empty canvas and
     creates titles at the clicked frame through the existing atomic title command.
   - Completed: expose Qt's Lock Transform To Source context action through an undoable neutral
     command. Standalone preview/export now resolves persistent-ID source chains at the same
     timeline frame, follows mask children automatically, fails safely on missing links/cycles,
     and excludes inherited transforms from the direct hardware bypass.
   - Completed: route Delete or Backspace from focused grading, opacity, transform, title,
     render-sync, and transcript rows to the existing neutral removal workflows. Focus targets are
     document-generation scoped and refreshed only while the row owns ImGui navigation focus;
     transcript targets additionally validate clip, active-cut path, and word identity before
     opening the existing confirmation modal, so stale focus cannot delete a timeline clip or row.
     Backspace never falls through to timeline-clip deletion, matching Qt's table-local behavior.
   - Completed: bind real context transcription to the shared WhisperX controller and route
     BiRefNet/SAM3 context actions into their functional Masks workflows. Metadata refresh remains
     undoable and grading/sync/transcript/speaker/face/proxy/Jobs/properties are directly reachable.
   - Remaining group-drag presentation and exact native menu styling polish.
   - Complete remaining keyboard-navigation/focus polish.

4. **Finish advanced inspector workflows.**
   - Completed: undoable per-row removal and Linear/Hold changes for grading, opacity, and
     transform keyframes.
   - Completed: keyframe/sync click-to-seek navigation plus undoable sync-marker
     frame/count/action editing and individual removal.
   - Completed: neutral title-keyframe text, placement, font, emphasis, opacity, color,
     interpolation, row load/seek, and removal editing.
   - Completed: atomic title-asset creation at the playhead with Qt defaults, stable IDs,
     exact canonical-`Titles` preference, conflict-safe numbered-lane routing, and no synthetic
     media item.
   - Completed: shared Qt-compatible curve sampling/LUT composition/simplification and Normalize
     Curves in both frontends, in addition to bounded point-table editing and shared three-point
     lock/smoothing behavior.
   - Grade final native styling polish.
   - Completed: promote correction polygon replacement to an undoable neutral command and bind
     creation, selection, enable/range edits, individual/all deletion, normalized vertex editing,
     Program-monitor click-to-draft/close/cancel overlays, and direct coalesced vertex dragging.
   - Completed: replace the stale "mask drawing" placeholder with Qt's actual generated-mask
     workflow: extract Qt-free sidecar identity/discovery/readiness validation, reuse it from Qt,
     promote durable sidecar/lock/Z fields through neutral JSON and both bridges, atomically
     materialize sidecar-owned child lanes with Qt defaults, restrict treatment edits to children,
     and expose explicit/automatic compositing Z order in ImGui.
   - Completed: extract SAM3 prompt-mask naming, paths, optimization arguments, environment,
     resumable manifest, subprocess cancellation, and validated result discovery into a Qt-free
     service. Qt consumes the shared launch plan; ImGui exposes the functional preflight controls,
     Jobs status/cancel, resume/restart, and shared-command sidecar materialization.
   - Completed: extract generic Qt-free process-group/stdin/log/cancel infrastructure plus shared
     WhisperX and BiRefNet plans/controllers. Qt reuses their path/command contracts; ImGui binds
     transcription stdin and artifacts plus BiRefNet preflight, progress, cancellation, and
     generated-matte materialization.
   - Completed: render BiRefNet's evolving live-preview artifact in Masks through the standalone
     decoder and a dedicated Vulkan image/view/descriptor with timestamp/size change detection,
     bounded decode dimensions, partial-write retry, and deterministic device-before-ImGui
     teardown.
   - Finish exact Qt correction tool-mode/focus and native-dialog polish.
   - Completed: direct title translation in the program monitor using the shared evaluated title
     state, output-space mouse mapping, visible bounds/center, live inspector synchronization, and
     one coalesced history transaction per drag.
   - Completed: extract Qt's anchored preview-resize calculation into a Qt-free helper reused by
     both shells; add a neutral preview-transform command with Qt-equivalent evaluated-to-relative
     temporal keyframe conversion; and bind ImGui Program-monitor move, axis-resize, and uniform
     resize handles with visible bounds, live preview refresh, and coalesced undo.
   - Completed: add a wrap-safe neutral pointer-angle helper and Program-monitor rotation handle;
     rotation supports Shift 15-degree snapping, live evaluated preview, and the same coalesced
     base-relative transform command as move/resize.
   - Completed: bind, normalize, serialize, bridge, interpolate, and standalone-render all
     Qt-known advanced title controls, including decoded pattern/logo assets; retain explicit
     coverage for their round trip into the legacy render timeline.
   - Completed: include title windows, patterned frames, and logos in standalone 3D
     orientation/depth/scale evaluation, matching Qt's element participation while retaining the
     documented CPU-versus-Vulkan raster/mesh approximation.
   - Completed: promote and bind the remaining Qt-known effect parameters (speech sync,
     difference/echo, and tiling), and render source tiling, difference matte, and temporal echo in
     standalone preview/export using the Qt geometry, frame-offset, smoothstep, and decay rules.
   - Completed: port Qt's generated-layout equations for news ticker, alternating-motion
     background, directional trim ticker, person orbit, freeze pattern, and step repeat to
     standalone preview/export, retaining count, scale, speed, direction, and animation behavior.
   - Completed: implement the remaining canonical effects in standalone preview/export: the 20
     single-pass shader modes use the Vulkan shader's UV, edge, neon, RGB-split, and halftone
     equations; 3D synth retains perspective projection, depth ordering, rotation, pulse, and
     opacity; progressive edge stretch scans the selected edge band across the canvas; and the
     three speaker-mask modes use an exact Euclidean distance field. Effect animation clocks honor
     render-sync adjustments and continuity across matching split clips.
   - Completed: extract Qt's master audio-dynamics model and processing order into a Qt-free core,
     alias the Qt preview settings to it, delegate Qt and standalone preview/export sample
     processing to the shared DSP, preserve exact legacy `audio*` fields, and bind functional
     ImGui controls through undoable runtime history.
   - Completed: move Qt's two-stage Harmonic Speech Isolation algorithm into the shared Qt-free
     Rubber Band core, preserve the exact legacy treatment ID, and bind it to standalone
     preview/export through an undoable ImGui selector.
   - Completed: resolve active transcript cuts through the shared cut-session core and apply Qt's
     per-word peak target, gain cap, boundary fades, and short-gap bridging in standalone
     preview/export; expose the persisted control through the shared dynamics command.
   - Complete title handle/selection polish and the remaining audio routing controls.

5. **Extract the remaining shared services.**
   - Completed: extract a read-only Qt-free transcript document/row projection core with
     unknown-field-preserving source JSON retention and focused strict-no-Qt coverage.
   - Completed: add a shared Qt-free cut-session/catalog adapter and wire ImGui cached
     path/version loading, active-cut persistence, rich-preview propagation, file-change
     invalidation, and clipped read-only rows. The Qt runtime registry takes precedence over a
     neutral persisted cut when the Qt user changes versions in-process.
   - Completed: move Qt `TranscriptTab` read-only row construction onto
     `TranscriptDocumentCore`, retaining stable Qt word IDs across asynchronous refreshes and
     deletion while reusing shared timing, gap, outside-cut, speaker, and edit-flag projection.
   - Completed: add a Qt-free, unknown-field-preserving word mutation and atomic-save core and bind
     ImGui mutable-cut text, raw source timing, skipped state, insert, neighbor expansion, original
     restore, render reorder, and confirmed deletion while guarding original/outside-cut rows.
   - Completed: bind speaker/text filters and shared cut create/rename/confirmed-delete.
   - Completed: move Qt primary text/timing mutation onto the neutral patch core and retain unknown
     word fields in the Qt in-memory adapter.
   - Completed: move Qt multi-row skip/delete, insert, neighbor expansion, original restore,
     arbitrary drag reorder, and create/rename/delete cut lifecycle onto the same neutral mutation
     and cut-session services used by ImGui. Shared batch deletion validates every stable address
     before removing in reverse physical order; Qt retains original-word IDs across structural
     rebuilds.
   - Completed: merge transcript/profile mutations into globally ordered runtime undo/redo using
     transient payload snapshots omitted from both neutral and legacy project serialization;
     atomically restore changed files and roll navigation back when external changes are detected.
   - Completed: replace the empty ImGui Speakers roster with neutral profile/word projection and
     unknown-field-preserving name, organization, and normalized title-location editing.
   - Completed: extract Qt-compatible `JCUTBOX1` JSON/compressed-CBOR face and identity artifact
     inspection; bind continuity-track review, diagnostics, selection, speaker assignment, clearing,
     anchor persistence, and global undo in ImGui.
   - Completed: extract a Qt-free face job controller that directly reuses the offscreen
     SCRFD/Vulkan executable, media-sidecar naming, manifest, logging, resume/cancel policy, and
     output validation; bind generation/cancel in Speakers; neutrally import Qt binary JSON and
     indexed qCompress/CBOR track output into the shared transcript `JCUTBOX1` artifact.
   - Completed: add source-accurate track-reference navigation and Program-monitor face-box
     visualization, guarded by persistent clip context.
   - Completed: extract the Qt continuity-avatar crop geometry into a shared Qt-free helper and
     use it for asynchronous selected-track source decoding plus an independently owned ImGui
     reference texture; extend it to a bounded eight-track strip with per-track diagnostics,
     retain the Qt adapter's existing rounded avatar treatment, and cover centered, edge-clamped,
     explicit-box, gap, and tile-copy behavior with regression tests.
   - Completed: extract contiguous speaker-section projection and inclusive range skip/unskip into
     a Qt-free core used by both shells; promote the per-clip minimum-word preference through
     neutral/core/legacy JSON and both bridges; bind an ImGui section table with assignment and
     rotation diagnostics, seek, undoable threshold changes, and transcript-history-backed edits.
   - Completed: extract section rotation/grading/mask option read-write into the same Qt-free core,
     preserve unknown section and assigned-track fields, reuse it from Qt, and bind an ImGui modal
     that persists the shared schema and creates section-boundary grading keyframes.
   - Completed: extract additive/replacement/clear section continuity-track assignment into the
     same core, including legacy primary-track synchronization and side-effect-free no-ops; reuse
     it from Qt's contiguous click assignment and expose per-row Assign/Clear actions in ImGui.
   - Completed: extract neutral source/transcript timeline mapping with source FPS, trim, playback
     rate, render-sync, and duration clamping; bind individual ImGui section export to an undoable
     exact-range command and the existing standalone export worker.
   - Completed: add the Qt-style qualifying-section bulk path: coalesce adjacent same-speaker
     rows, normalize track IDs, derive deterministic safe names, skip existing/duplicate/unmapped
     jobs, and reuse one cancellable sequential worker with live batch index/completion/failure
     state.
   - Completed: render assigned continuity-track avatars in each ImGui section row through one
     bounded asynchronous strip texture; reuse the neutral Qt-compatible crop and strip geometry,
     share UV slicing in the core, and invalidate the texture on source or track-anchor changes.
   - Completed: extract Qt-free, skipped-range-aware speaker-introduction generation and all five
     fly-in styles; reuse the animation core from Qt, bind ImGui controls, and atomically replace
     durable generated title clips on conflict-free dedicated lanes with undo/redo.
   - Completed: extract deterministic person-name/organization/spurious-speaker mining with
     confidence/rationale and unknown-field-preserving application; reuse it from Qt and bind an
     explicit selectable review/apply table through ImGui transcript history.
   - Completed: extract proxy candidate discovery and file/image-sequence usability policy; bind
     undoable attach/discover/use/disable/clear controls and keep generated mask children
     normalized to their source without deleting sidecar files.
   - Completed: create JPEG image-sequence, H.264 MP4, and Motion JPEG MOV proxies through a
     cancellable Qt-free job that reuses standalone timeline rendering and FFmpeg encoding,
     requires explicit consent before replacement, writes processing manifests, resumes JPEG
     sequences after the highest durable frame, and offers attachment through the neutral command.
     Confirmed deletion accepts only the three exact source-adjacent candidates, refuses symlinks,
     removes container manifests, and clears a matching project association.
   - Completed: match the Qt Explorer's open-folder/copy-absolute-path actions and reuse the
     standalone decoder plus generic Vulkan auxiliary upload path for non-blocking media
     hover/selection thumbnails.
   - Completed: list export, face detection, and proxy generation independently in Jobs, provide
     separate cancellation, and surface their progress/output/manifest/status diagnostics.
   - Completed: extract a Qt-free libcurl gateway contract for entitlement, usage, license rows,
     task submission, Supabase-direct fallback, and response parsing; bind asynchronous ImGui
     access refresh, model-backed project-context chat, project budget accounting, and compatible
     non-secret project settings. Qt's gateway URL helper reuses the neutral normalizer.
   - Completed: add a Qt-free compatible credential store for load/save/sign-out through the same
     Linux `secret-tool` service/fields and permission-restricted config fallback as Qt; move the
     Qt editor's credential methods onto it.
   - Completed: add neutral asynchronous Supabase refresh-token exchange, secure-store update, a
     manual ImGui refresh action, and automatic refresh after entitlement auth rejection.
   - Completed: add a cancellable Qt-free browser-PKCE login with an ephemeral loopback callback,
     RFC 7636 S256 challenge, system browser launch, code exchange, secure session persistence,
     and deterministic challenge/URL tests.
   - Completed: create and open subscription checkout asynchronously using Qt's environment-first
     product-slug fallback order.
   - Completed: share cloud speaker-mining payload/response projection, submit it from ImGui, and
     route inferred names/organizations into the existing selectable neutral proposal table and
     transcript undo path.
   - Completed: add an ImGui account identity/access badge and a bounded AI activity table covering
     the same operational phases as Qt's activity window, including login, refresh, entitlement,
     checkout, chat, speaker mining, and credential events.
   - Completed: promote generator control-window/live-preview and explicit resume-checkpoint restart
     into the shared face-job request/manifest/command and bind the controls in ImGui.
   - Completed: add explicit source/proxy FaceDetections input selection while retaining
     source-anchored sidecars, selected-clip grading via the shared Qt-compatible legacy clip
     projection, and the real generator topology benchmark with durable/reloadable
     `launch_control.json` recommendations.
   - Complete native account/activity presentation polish, deeper identity decisions, and
     remaining face diagnostics. Dialogue-only scanning is not listed as a
     parity blocker because the current Qt generator does not implement the behavior advertised by
     its stale help text.
   - Completed: bind Qt-compatible autosave interval/count and history entry/size preferences,
     write atomic unknown-field-preserving `state_backup_*.json` snapshots without touching active
     state/history, trim them under the project lock, and expose persisted audio-buffer selection
     with safe RtAudio stream restart plus requested/actual diagnostics.
   - Completed: extract Qt-free decoder preference/threading normalization and reuse it from the Qt
     adapter and standalone preview/proxy/export paths; bind persisted ImGui decode mode,
     H.264/H.265 threading, deterministic decode, and shared lane preference; provide functional
     preview decoder refresh, requested/effective software-fallback diagnostics, and an
     asynchronous source decode benchmark.
   - Completed: extract FFmpeg hardware-device creation and pixel-format selection into a Qt-free
     core reused by the Qt format adapter and standalone decoder; make ImGui preview/proxy/export
     attempt native hardware decode, transfer frames to the current CPU compositor, recover from
     device/open/frame failures by reopening software, and surface effective device/fallback
     diagnostics in System, Pipeline, control snapshots, and benchmarks.
   - Completed: persist and apply an explicit Auto/CUDA/VA-API/VideoToolbox/D3D11VA/DXVA2 decode
     device preference through the shared neutral policy and FFmpeg device-order core; changing it
     rebuilds the standalone preview source cache and benchmark/export requests use the same choice.
   - Completed: expose RtAudio stereo-output discovery and stable-name selection through the
     Qt-free audio facade; persist system-default or explicit selection, safely close/restart an
     open stream, refresh devices on demand, and report requested/active output diagnostics.
   - General processing jobs, decoder prefetch/cache controls, startup/profile benchmark
     automation, direct zero-copy composition, concurrent lane scheduling, richer diagnostics,
     and remaining preferences.

6. **Close preview/export equivalence.**
   - Completed: Qt-free FFmpeg clip decoding, neutral 48 kHz stereo mixing, gain/pan/mute/solo,
     source timing, playback-rate mapping, render-sync mapping, export-range and speed mapping,
     edge fades, container audio-stream creation, sample-format conversion, encoding, and muxing.
   - Completed: extract the Rubber Band implementation into a Qt-free shared service used by both
     the Qt wrapper and standalone export; apply it to clip and export playback speed with pitch and
     encoded-duration coverage.
   - Completed: make the standalone exporter honor the authoritative bounded/discontiguous
     `exportRanges` instead of expanding a short request to timeline end; concatenate each range's
     video, continuously numbered images, progress segments, and audio while buffering codec-sized
     audio frames across segment boundaries.
   - Completed: derive each export clip's canonical source-sample envelope from the authoritative
     export ranges, seek FFmpeg to that envelope, retain only intersecting 48 kHz samples plus
     decoder/stretch padding, preserve absolute source offsets through clip-rate Rubber Band
     processing, and stream export-speed stretching through bounded provider/sink blocks directly
     into codec-sized audio buffering instead of materializing a full segment mix and stretched
     copy.
   - Completed: extract a bounded Qt-free source-range envelope core with absolute source-offset
     and clip-rate cache scaling, expose the existing asynchronous ImGui decoded-audio cache through
     a read-only clip-envelope query, warm that cache while paused when waveforms are visible, and
     draw bounded per-clip timeline envelopes under the shared global/track visibility policy.
     Qt-versus-neutral step-amplitude fixtures compare both source halves while explicitly
     preserving Qt's conservative pyramid-bin boundary behavior.
   - Completed: add a GPU-soft identical-neutral-project harness that feeds the same document
     through Qt's Vulkan preview and the Qt-free standalone renderer. Its five current feature
     fixtures cover real image decode, brightness/contrast/saturation/opacity grading, a translated
     translucent title/window, manual mask matte, generated grid tiling, temporal echo over a real
     generated MPEG-4 source, a persisted active-cut transcript with active-word highlighting, and
     baked/retargeted speaker framing. A seventh fixture feeds identical synthetic decoded clips
     through the Qt export mixer and standalone mixer and requires sample-exact agreement for
     two-clip track/clip gain, timeline mapping, edge-fade, summing, and clamping. The baseline
     composition acceptance budget is mean RGB error below 10 levels and more than 80% of pixels
     within 16 levels; the measured title fixture has 82.73% within-budget pixels because the two
     text/window raster paths antialias edges differently. The speaker fixture keeps aggregate
     mean error below 9 while allowing the CPU and Vulkan bilinear samplers to round transformed
     gradient samples differently.
   - Completed discrepancy fix: Qt offscreen preview now resolves the durable neutral
     `transcriptActiveCutPath` before falling back to the process-global active-cut registry. An
     identical project therefore renders its persisted transcript in either frontend, including
     headless/offscreen comparison where no Qt editor session populated that registry.
   - Extend the identical-project harness across encoded export output; treat measured text
     antialiasing, video decoder/color conversion, and transformed-sampler deltas as explicit,
     bounded renderer exceptions unless the underlying backends are unified.

7. **Add shell and cross-shell parity automation.**
   - Complex project roundtrips without state/history loss.
   - Shortcut and interaction smoke tests.
   - Detection of visible but unwired controls.
   - Equivalent snapshots, preview frames, and exports after identical command sequences.

## Verification

Builds must use the repository wrapper:

```bash
./build.sh --target jcut_imgui
./build.sh --target editor
./build.sh --with-tests --target test_editor_runtime
./build.sh --with-tests --target test_imgui_project_history
./build.sh --with-tests --target test_imgui_standalone_export
./build.sh --with-tests --target test_imgui_standalone_render
./build.sh --with-tests --target test_imgui_neutral_boundary
./build.sh --with-tests --target test_grading_keyframes
./build.sh --with-tests --target test_media_drag_drop
./build.sh --with-tests --target test_imgui_vulkan_import_boundary
./build.sh --with-tests --target test_frame_payload_core
./build.sh --with-tests --target test_frame_handle
./build.sh --with-tests --target test_transcript_document_core
./build.sh --with-tests --target test_transcript_cut_session_core
./build.sh --with-tests --target test_transcript_tab_follow
./build.sh --with-tests --target test_audio_mix_policy
./build.sh --with-tests --target test_audio_time_stretch
./build.sh --with-tests --target test_audio_time_stretch_cache
./build.sh --with-tests --target test_preview_geometry
./build.sh --with-tests --target test_media_color_passthrough
./build.sh --with-tests --target test_media_root_contract
```

The acceptance gate is the unfiltered wrapper command, not only selected targets:

```bash
./build.sh
```

The speaker-section export checkpoint is covered by the shared Qt-free section-core assertions
and the existing runtime/render suites. Qt and ImGui now call the same adjacent-section
coalescing, track normalization, title, filename, and playback-speed suffix policy. ImGui builds
one exact range-aware document snapshot per non-conflicting output and processes those snapshots
sequentially through its existing cancellable exporter while reporting the current job and
aggregate completed/failed counts. The focused verification for this checkpoint passed:

- `test_transcript_document_core`: shared section coalescing, normalized track IDs, deterministic
  title/filename generation, and speed suffix assertions passed.
- `test_editor_runtime`: 75 passed.
- `test_imgui_standalone_export`: 14 passed, including exact bounded/discontiguous video and audio
  concatenation.
- `test_imgui_standalone_render`: 25 passed.
- `python3 tests/check_imgui_binary_no_qt.py build/jcut_imgui`: passed.
- `git diff --check`: passed.

The section-avatar checkpoint keeps decode work off the UI thread, shares one atlas across visible
rows, bounds the atlas to 24 continuity tracks, and reuses `faceAvatarCropRectCore`,
`cropFaceAvatarImageCore`, `faceAvatarStripImageCore`, and the new tested
`faceAvatarStripUvCore`. `test_imgui_standalone_render` remains 25/25 with first/gap/last/out-of-
range UV assertions, and `./build.sh --target jcut_imgui` passes.

This full build passed after restoring the direct `Qt6::Network` dependency of
`jcut_vulkan_facedetections_offscreen` and moving the `ProjectManager`-dependent startup source
out of the lower-level runtime-support archive. It passed again after the shared track
media-presence policy and timeline-row visual/audio gestures were added; the exact acceptance
command remained `./build.sh` with no options. The same unfiltered command passed again after
transport/audio-view parity, the live Pipeline graph, direct grading-curve manipulation, and the
shared asynchronous Auto Oppose workflow were added. It passed once more after the expanded
timeline workflow navigation, Qt-domain grade correction, and undoable source-metadata refresh.
The exact `./build.sh` command passed on 2026-07-23 after the shared AI/access and credential
services, cloud speaker-review flow, FaceDetections benchmark/source/proxy/grading preflight, and
Qt-compatible clip projection were integrated. It passed again after Media Explorer filesystem
actions and asynchronous thumbnails, AI identity/activity presentation, direct transformed and
opacity-keyed hardware presentation, and CUDA-NV12 base brightness/contrast/saturation conversion
were integrated. That run also caught and corrected an overloaded-member ambiguity in
`test_imgui_neutral_boundary`; the exact unfiltered rerun then completed all 42 remaining build
steps successfully. The optional Qt CorePrivate/GuiPrivate messages remained warnings only.
The next focused checkpoint extended the same CUDA-NV12 handoff to interpolated
shadow/midtone/highlight values and grading keyframes. Its targeted `jcut_imgui`,
`test_shader_grading_logic`, and `test_imgui_standalone_render` builds and tests passed. The exact
unfiltered `./build.sh` command then completed all 121 remaining build steps successfully. The
subsequent curve-LUT checkpoint adds the shared packed RGB/luma table and Vulkan storage-buffer
path described above. Its focused tests, strict no-Qt check, and the exact unfiltered `./build.sh`
command passed; the final wrapper run completed all 124 remaining build steps successfully.
The following source/output aspect-fit checkpoint also passed `test_editor_runtime` (74),
`test_imgui_standalone_render` (24), the strict final-binary no-Qt check, and the exact unfiltered
`./build.sh`; that wrapper run completed all 96 remaining build steps successfully.
The shared token-profile/remote-avatar checkpoint passed `test_ai_gateway_core`, the strict no-Qt
checks for both that core test and `jcut_imgui`, focused `jcut_imgui` and `editor` builds, and the
exact unfiltered `./build.sh`; that wrapper run completed all 89 remaining build steps
successfully.
The selected continuity-reference checkpoint passed its 25-case standalone-render suite, the
strict final-binary no-Qt check, `git diff --check`, and focused `jcut_imgui` and `editor` builds.
The exact unfiltered `./build.sh` command then completed all 103 initially reported build steps
successfully; the optional Qt CorePrivate/GuiPrivate messages remained warnings only.
The first cross-shell command-script checkpoint then passed all 75 `test_editor_runtime` cases
after canonicalizing the shared image-sequence preference default. It compares neutral and legacy
snapshots after every scripted action and their final Qt-compatible save payloads. The exact
unfiltered `./build.sh` command then completed all 106 initially reported build steps
successfully; the optional Qt CorePrivate/GuiPrivate messages remained warnings only.
The bounded multi-track reference-strip checkpoint retained the 25 passing standalone-render
cases and strict no-Qt final-binary result. The exact unfiltered `./build.sh` command then
completed all 103 initially reported build steps successfully.
The shared contiguous-speaker-section checkpoint passed the neutral transcript assertions, all
75 `test_editor_runtime` cases, focused Qt and ImGui builds, the strict no-Qt final-binary check,
and `git diff --check`. The exact unfiltered `./build.sh` command then completed all 140 initially
reported build steps successfully; the optional Qt CorePrivate/GuiPrivate messages remained
warnings only.
The section-options checkpoint added shared rotation/grading/mask persistence, Qt adapter reuse,
the ImGui options modal, and section-boundary grading commands. Its focused
`test_transcript_document_core` assertions and all 75 `test_editor_runtime` cases passed, as did
focused Qt and ImGui builds, the strict no-Qt final-binary check, and `git diff --check`. The JSON
reader regression test also caught a dangling reference in the first implementation; the shared
lookup now returns an owned optional section before any option fields are read.
The exact unfiltered `./build.sh` acceptance command then passed all 106 initially reported build
steps; the optional Qt CorePrivate/GuiPrivate messages remained warnings only.
The per-section continuity-track assignment checkpoint then passed the expanded neutral transcript
assertions (add, replace, clear, extension preservation, legacy-field synchronization, malformed
container fallback, and side-effect-free no-op), all 48 `test_transcript_logic` cases, focused Qt
and ImGui builds, the strict final-binary no-Qt check, and `git diff --check`. Qt's contiguous
click-assignment path and ImGui's per-row Assign/Clear actions now use the same core mutator. The
final exact unfiltered `./build.sh` rerun passed all 106 initially reported build steps; the
optional Qt CorePrivate/GuiPrivate messages remained warnings only.
The bounded/discontiguous export checkpoint corrected the standalone exporter's former full-
timeline expansion, added continuous video/image/progress/audio concatenation across neutral
ranges, moved standalone source-frame selection onto source-FPS-aware mapping, and bound individual
speaker-section export in ImGui. Its focused exporter suite now has 14 passing cases, including a
two-segment encoded video/audio fixture and 24-fps transcript-section mapping; all 75 runtime tests
and all 25 standalone-render tests also pass.
The exact unfiltered `./build.sh` acceptance rerun then passed all 168 initially reported build
steps; the optional Qt CorePrivate/GuiPrivate messages remained warnings only.

The bounded-audio checkpoint now computes per-clip source envelopes before decode, seeks and clips
decoded samples using stream timestamps while retaining their absolute canonical source position,
and keeps clip-rate mapping valid after Rubber Band processing. Export playback-speed conversion
uses the shared bounded-memory two-pass Rubber Band provider/sink API and sends output directly to
the existing codec-frame buffer. The focused standalone-export suite has 15 passing cases,
including a ten-second source fixture that exports only a late two-frame range at 2x clip rate,
proves the cache starts at the exact requested eight-second source sample, retains less than one
second, and remains audible through the canonical mixer.
The focused `./build.sh --target jcut_imgui` build, strict final-binary no-Qt linkage check, and
`git diff --check` passed. The exact unfiltered `./build.sh` acceptance rerun then completed all
130 remaining build steps successfully; the optional Qt CorePrivate/GuiPrivate and NCNN AVX VNNI
messages remained warnings only.

The timeline-waveform checkpoint added the shared Qt-free absolute-source envelope service and
reused the ImGui audio runtime's already-decoded cache, so waveform display does not introduce a
second media decoder. Paused timelines now warm audio asynchronously when waveform visibility is
enabled; each audible clip draws a bounded 16-to-512-column envelope honoring global and per-track
visibility, source-in/rate mapping, and processed-cache offsets. `test_waveform_service` passes all
6 cases including direct Qt-versus-neutral two-amplitude range comparisons and offset/scaled-cache
coverage; all 25 standalone-render cases and the focused ImGui executable build also pass.
The strict final-binary no-Qt linkage check and `git diff --check` passed. After excluding the new
shared implementation from the legacy root source glob so it is compiled exactly once, the final
exact unfiltered `./build.sh` acceptance rerun completed all 48 remaining build steps successfully;
the optional Qt CorePrivate/GuiPrivate and NCNN AVX VNNI messages remained warnings only.

The identical-project render-comparison checkpoint has a dedicated GPU-soft cross-frontend test
target. The same neutral documents now exercise real image decode, tonal grading, opacity, a
translated translucent title/window, a manual split mask, generated grid tiling, temporal echo
over a generated moving MPEG-4 source, a persisted active-cut transcript with highlighted active
word, and baked/retargeted speaker framing through both Qt Vulkan and standalone ImGui
composition. All six feature fixtures pass their measured budgets (eight QtTest cases including
setup/cleanup). A seventh sample-level fixture feeds the same two decoded clips through the Qt
export mixer and standalone mixer and requires exact agreement for track/clip gain, timeline
mapping, edge fades, summing, and clamping (nine QtTest cases including setup/cleanup). The temporal
fixture separately proves that the effect visibly changes both
renderers, then bounds effect-output agreement relative to the plain-video decoder/color-conversion
baseline. The baseline title fixture remains below
10 mean RGB levels and above 80% within 16 levels; its observed 82.73% quantifies the
title/window antialiasing boundary. The transformed speaker fixture remains below 9 mean RGB
levels while using a lower per-pixel threshold because Vulkan and CPU bilinear sampling round
off-grid gradient samples differently. This checkpoint also found and fixed a real Qt offscreen
state bug: transcript rendering now prefers the clip's persisted neutral
`transcriptActiveCutPath`, with the process-global active-cut registry retained only as fallback.
Encoded-output cross-renderer comparison remains open and is not claimed by this checkpoint.

Focused test results for this checkpoint:

- `test_editor_runtime`: 75 passed; focused coverage includes guarded undoable project-media removal, neutral
  razor splitting, media-lane conflict routing, atomic filesystem-drop insertion/undo coverage, and
  channel-scoped keyframe removal, atomic keyframe reframe/value edits, full neutral
  title/transcript field round trips, normalized undoable correction-polygon replacement,
  Qt-equivalent base-relative preview-transform keyframe commits, mask-child owner propagation,
  shared anchored-resize math, undoable generated-sidecar materialization with Qt ownership/Z
  defaults and durable-field JSON round trips, mixed transcript/project global history ordering
  with transient-payload serialization exclusion, atomic
  title creation/default/render/undo coverage with
  media-bin isolation and ImGui source wiring, Qt-known grading reset with unknown-field
  preservation, canonical mask-owner sync normalization, parent-authoritative cache normalization,
  rejected direct child timing edits, relationship-safe aggregate move/nudge/trim/resize/rate,
  copy/cut/paste/delete/split, and marker movement/remapping, count bounds, scoped sync marker
  removal, clip lock enforcement, playback-rate ripple behavior, shared Scale-to-Fill aspect math
  and undo/redo behavior, plus neutral/Qt effect-preset catalog equivalence,
  reverse/stationary effect-speed bounds, and undoable grading curve sanitization, tonal clamping,
  locked RGB regeneration, unlocked curve preservation, generated-child JSON/bridge round trips,
  malformed-lane recovery, indexed normal-lane insertion, guarded child targets, source/child
  reorder and delete behavior, independent child visual/grade/height state, undo restoration, and
  atomic Qt-compatible track crossfades with subframe overlap, mask-child timing, overflow guards,
  generated-lane rejection, undo/redo, Qt-compatible undoable multi-segment export-range
  set-start/set-end/split/reset semantics, cycle-safe source-transform inheritance/locking, and
  shared Qt-compatible empty/audio-only/visual-only/audiovisual/legacy track media-presence policy,
  exact 10–300% transport bounds, shared disjoint-range stepping, gap-skipping tick advancement,
  loop-to-first-range, stop-at-final-range, and restart-at-valid-range semantics
  plus shared Qt/ImGui Auto Oppose RGBA-statistics thresholds, event spacing, adjustment clamps,
  event generation, Qt's full `-10…10` base/keyframe grading command domain, and a cross-shell
  command script with per-action canonical snapshot and final legacy-save equivalence
- `test_imgui_project_history`: 30 passed after lifecycle and history-navigation additions,
  including New/Save As/Rename, symlink-escape rejection, atomic activation, stale-session
  rejection after rename, marker-commit cleanup, post-commit load rollback, rename recovery,
  legacy/delta entry navigation, redo preservation, stale/escaped rejection, paired rollback,
  concurrency-validated legacy transcript overrides, and History-tab wiring
- `test_imgui_standalone_export`: 14 passed, including exact bounded/discontiguous video/audio
  range concatenation, progress segment identity, source-FPS-aware section mapping, and explicit
  output-format muxer selection when
  the filename extension disagrees and Qt-free synthetic audio mixing coverage for timeline/source
  mapping, render-sync adjustment, gain/pan/mute policy, and clip edge fades, plus real FFmpeg WAV
  decoding, pitch-preserving clip/export-speed infrastructure, AAC encoding, shortened audio/video
  duration agreement, MP4 muxing, encoded-audio-packet verification, retained 440 Hz pitch at 2x,
  transcript-aware normalization, shared harmonic isolation/treatment, shared master dynamics, and
  neutral proxy generation
- `test_imgui_standalone_render`: 25 passed, including neutral facade/source-boundary, standalone
  stream probing with still and image-sequence dimensions, image-sequence-directory probe/render
  with source-in and playback-rate mapping,
  ordered two-layer alpha composition, animated opacity/grading/transform/title rasterization,
  UTF-8-capable Fontconfig/FreeType title styling, advanced title windows/frames, decoded image
  materials and logos, shadow, extrusion and 3D controls, hidden/force-opaque track modes,
  strict mask-matte alpha/correction rendering, advanced masked grading/curves, foreground/repeat
  layers, drop shadows, missing-sidecar fail-closed behavior, source tiling pattern output, and
  mapped-frame difference-matte/temporal-echo rendering, and parameter-sensitive animated output
  for generated layouts and 3D synth, all 20 single-pass shader modes, progressive edge stretch,
  all three speaker-mask distance-field modes, skip-aware effect-clock behavior, and a real
  JSON-backed active-cut transcript overlay on an audio-only timeline with timed word highlighting,
  UTF-8-capable typography, background/shadow styling, and normalized placement,
  shared baked/face-box speaker-framing contracts, hardware-bypass rejection for framed clips,
  and a real transcript-driven automatic-framing render backed by a `JCUTBOX1` continuity artifact
  plus an active section assignment/rotation,
  Scale-to-Fill shell wiring, visible canonical-owner marker hit-testing, reload-safe marker
  popup/modal contracts, stable-path audio-topology replacement, idle audio timeline status,
  derived-sidecar refresh coverage, Grade direct curve-canvas/point/lock/smoothing/normalization
  and decoded-source Auto Oppose source contracts, and generated-child presentation, drop,
  reorder, drag-affordance, height, and
  grading-preview contracts, plus requested/effective decode-policy fallback, deterministic
  software-thread selection, standalone source benchmark reporting with hardware-device/fallback
  fields, and the Qt-free audio facade's persisted output-device selection/status contract,
  plus a fake-worker end-to-end SAM3 prompt-mask fixture covering Qt-compatible job/artifact
  naming, optimization arguments, cache/job environment, manifest lifecycle, validated sidecar
  results, ImGui launch/materialize/cancel wiring, and timeline-row visual/audio toggle source
  contracts backed by the shared media-presence policy, plus exact centered, edge-clamped, and
  explicit-bound reference-avatar crop geometry shared by Qt and ImGui
- `test_shader_grading_logic`: 20 passed, including the CUDA-NV12 handoff shader's canonical RGBA
  output, base grading push contract and application stage, Vulkan/GL advanced grading behavior,
  mask preprocessing, and CPU grading/matte fallbacks
- `test_imgui_standalone_render`: the current 25-test run additionally proves direct-hardware
  eligibility for presentation transforms, opacity, force-opaque tracks, full tonal/curve grading,
  baked/dynamic speaker framing, upper title/decoded/effected layers, and transcript-overlay
  composition; it also proves transparent subcomposition preserves source color and half opacity
  while same-track/below-base overlaps remain ineligible
- `tests/check_imgui_binary_no_qt.py build/jcut_imgui`: passed after the latest media, access, and
  direct-render work and reports `jcut_imgui has no Qt linkage`
- `test_processing_job_manifest`: 11 passed, including direct Qt job-root/manifest-path versus
  neutral prompt-mask plan parity, the shared SAM3 output/argument contract, an interactive
  WhisperX fake worker with stdin/log/manifest/output validation, and a BiRefNet fake worker with
  command/cache/progress/log/manifest/alpha-output validation
- `test_vulkan_text_generation`: 16 passed, including a direct four-corner Qt `QMatrix4x4`
  Vulkan-title MVP versus neutral title-projection comparison under combined off-center
  yaw/pitch/roll/depth/scale perspective
- `test_imgui_neutral_boundary`: executable and strict no-Qt linkage check passed; its compile-time
  contract covers the public audio facade, frame importer, preview, and editor-runtime types, and
  its runtime assertions cover base, interpolated, held, and endpoint full-grading evaluation plus
  curve sanitization, non-finite Qt bounds, deterministic LUT/normalization behavior, and shared
  three-point tone/curve mapping, shared decode aliases/parsing, hardware/zero-copy fallback,
  explicit hardware-device alias/order selection, H.264/H.265 thread-mode normalization,
  deterministic single-thread policy, and the public
  output-device refresh/selection facade signatures. It also links the neutral FFmpeg hardware
  device core, verifies hardware pixel-format selection, and remains guarded by the strict no-Qt
  linkage check
- `test_grading_keyframes`: 15 passed, including direct Qt-versus-neutral evaluator parity for
  endpoints, interpolation, hold behavior, all nine tonal fields, curve topology fallback,
  curve flags, independently keyed opacity, fixed golden linear/smoothed LUTs, Qt wrapper parity,
  bounded normalization, and non-finite input handling
- `test_media_drag_drop`: 22 passed, retaining Qt generated-mask-track reconciliation, hierarchy
  reorder, aggregate ownership, clipboard, split, deletion, and presentation coverage
  plus direct Qt `TimelineWidget` versus shared export-range-core parity
- `test_imgui_vulkan_import_boundary`: executable and strict no-Qt linkage check passed for the
  real external-frame importer facade and its shared Vulkan core
- `test_frame_payload_core`: standalone frame ownership/lifetime assertions and the strict no-Qt
  linkage check passed
- `test_frame_handle`: 11 passed, including the source-compatible QRhi adapter API, null CPU
  payload behavior, shared ownership, crop preservation, and memory accounting
- `test_transcript_document_core`: standalone projection assertions, speaker-framing profile
  location/box/confidence interpolation, and the strict no-Qt linkage check passed
- `test_transcript_cut_session_core`: added for source/status/audio-stream resolution, editable
  creation, v0/v1/v2/v10 numeric catalog ordering, custom labels, persisted custom/stale-cut handling,
  outside-cut rows, file-stamp invalidation, neutral command/JSON round trips, rich-render wiring,
  unknown-field-preserving word text/timing/skip mutation, insert/expand/restore/reorder semantics,
  deletion bookkeeping, cut create/rename/delete guards, atomic-save cleanup, ImGui action wiring,
  Qt-compatible compressed face/identity artifact inspection, continuity-track assignment/clearing,
  active-section and identity assignment precedence, source-relative continuity sampling, section
  rotation propagation, and robust outlier-resistant framing smoothing,
  Qt-compatible face-job sidecar/command construction, generator binary-envelope plus indexed
  qCompress/CBOR import, unknown-field preservation, source/rate-aware reference navigation,
  neutral profile-styled speaker-change title generation, atomic lane placement/replacement and
  undo, deterministic name/organization/spurious-label proposal and scoped application, and strict
  no-Qt linkage. Proxy coverage also exercises Qt-compatible candidate discovery, regular-file
  usability, undoable source association, generated mask-child propagation, direct-child
  rejection, and undo; its assertions and strict no-Qt linkage check passed
- `test_effect_presets`: 53 passed after the Qt speaker fly-in adapter was moved onto the neutral
  animation core, retaining speaker-change/title styling, skipped-range timing, four directional
  fly-ins, 3D wrap keyframes, generated placement/replacement, and transcript-parent resolution
- `test_imgui_standalone_export`: 15 passed, including bounded/discontiguous segment export,
  late-range bounded audio seeking/retention with absolute source-offset and clip-rate mapping, and
  end-to-end Qt-free JPEG/H.264/MJPEG proxy
  generation, source probing, exact bounded frame output, JPEG resume/offset append, manifests,
  container readability, explicit overwrite refusal, path-constrained deletion, and unrelated-path
  rejection, standalone video/image-sequence export, mixed/encoded audio, playback-speed duration
  and pitch preservation, transcript normalization, harmonic isolation/treatment, shared master
  dynamics, and neutral proxy generation; the generated H.264 proxy is also decoded under an
  explicit CUDA preference and proves either an active named CUDA device or successful runtime
  software fallback
- `test_imgui_project_history`: 31 passed, including Qt-compatible autosave snapshot naming,
  unknown-field/override preservation, non-mutation of active state/history, project-lock
  serialization, and oldest-first bounded retention
- `test_transcript_tab_follow`: 16 passed, including shared-core read-only row projection and stable
  Qt word identity after deletion in addition to follow, gap, outside-cut, overlay-refresh, version
  deletion, and speech-filter behavior
- `test_audio_mix_policy`: 15 passed, including Qt export/preview clip-fade equivalence, atomic timeline replacement, stale mixer and
  time-stretch rejection, generation-scoped terminal warmup failure, source-cache invalidation,
  and ring-buffer reset coverage
- `test_audio_time_stretch`: 6 passed after moving the Rubber Band implementation to the shared
  Qt-free core, including disabled-fallback policy, long-input streaming, exact offline duration
  normalization, and two-stage speech-harmonic transport duration
- `test_audio_time_stretch_cache`: 9 passed, including canceled stale writes and guarded commit
  refusal without damage to an existing sidecar
- `test_preview_geometry`: 16 passed after separating QWidget-only preview transform helpers
- `test_imgui_binary_no_qt_widgets`: passed for the current `jcut_imgui`
- `test_imgui_binary_no_qt_network`: passed for the current `jcut_imgui`
- `test_imgui_binary_no_qt_concurrent`: passed for the current `jcut_imgui`
- `test_media_color_passthrough`: 11 passed, including ordered/clamped parallel image-sequence
  batch loading
- `test_media_root_contract`: 8 passed; preserves the ImGui `mediaItems` extension across Qt state
  application and serialization in addition to the shared workspace-root contracts
- `test_imgui_cmake_qt_boundary`: passed; the generated ImGui runtime target interfaces, including
  `jcut_editor_core`, expose no Qt Widgets, Qt Network, Qt Concurrent, Qt GuiPrivate, or
  `jcut_frame_handle_qrhi_adapter` dependency

The generated MOC units also show the intended separation:

- `jcut_preview_decode_runtime`: `AsyncDecoder`, `MemoryBudget`, and `TimelineCache`
- `editor_test_support`: `ProjectManager` and `TranscriptTab`

The focused boundary checks now pass:

```bash
python3 tests/check_imgui_binary_no_qt.py --component Widgets build/jcut_imgui
python3 tests/check_imgui_binary_no_qt.py --component Network build/jcut_imgui
python3 tests/check_imgui_binary_no_qt.py --component Concurrent build/jcut_imgui
python3 tests/check_imgui_cmake_qt_boundary.py build/tests/imgui_runtime_link_interfaces.txt
python3 tests/check_imgui_binary_no_qt.py build/tests/test_imgui_neutral_boundary
python3 tests/check_imgui_binary_no_qt.py build/tests/test_imgui_vulkan_import_boundary
python3 tests/check_imgui_binary_no_qt.py build/tests/test_frame_payload_core
python3 tests/check_imgui_binary_no_qt.py build/tests/test_ai_gateway_core
python3 tests/check_imgui_binary_no_qt.py build/tests/test_transcript_document_core
python3 tests/check_imgui_binary_no_qt.py build/tests/test_transcript_cut_session_core
```

The strict final-binary check now passes:

```bash
python3 tests/check_imgui_binary_no_qt.py build/jcut_imgui
```

It reports `jcut_imgui has no Qt linkage`; Qt6 Gui/Core/DBus are no longer in the executable's
dependency closure.

Qt-free linkage does not establish functional parity. Rich preview composition/Vulkan production
and the incomplete workflows in the parity matrix remain exit blockers.

## Parity Exit Criteria

The ImGui implementation is on par only when all of the following are true:

- `jcut_imgui` passes the strict no-Qt binary check.
- Every supported Qt editor surface has a connected ImGui equivalent.
- No visible ImGui control is presented as functional while discarding user input.
- Equivalent Qt and ImGui command sequences produce equivalent project snapshots.
- Complex projects roundtrip between shells without state or history loss.
- Preview and export results are equivalent for the same project and settings.
- Timeline editing, shortcuts, drag/drop, context actions, and undo/redo are functionally
  equivalent.
- Layout, preferences, dirty-state handling, and project lifecycle behavior are covered by smoke
  tests.
- Shared parity and shell-level test suites pass.

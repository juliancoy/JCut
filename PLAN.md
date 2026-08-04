Biggest errant couplings
Shared editor catch-alls
[editor_shared_core.h (line 1)](/home/julian/Documents/JCut/editor_shared_core.h:1) is PageRank #1 with 37 include consumers. It combines media probing, transcript structures, speaker profiles, overlay layout, visual effects, fonts, and audio timing constants.
The nine-line editor_shared.h remains PageRank #3 with 55 consumers across nine owners. Together these headers make unrelated subsystems rebuild and depend on one another.
Next refactor: split editor_shared_core.h into narrow media, transcript, speaker, overlay, effects, and audio contracts, then eliminate the remaining umbrella consumers owner-by-owner.
Production code depends on editor_test_support
The target defined at [CMakeLists.txt (line 977)](/home/julian/Documents/JCut/CMakeLists.txt:977) combines project management, transcript widgets, detector settings, face UI, preview transforms, and profiling.
Production dependencies include:
editor_core: 31 file edges
Vulkan face tool: 5
Qt application: 4
Project CLI: 1
This is not really test support. It should be split into focused widget-support targets so consumers do not acquire Qt Widgets, Qt Concurrent, transcript UI, project management, and detector UI as one package.
Preview contract ownership is inverted
[preview_interaction_state.h (line 1)](/home/julian/Documents/JCut/preview_interaction_state.h:1) is owned as UI but consumed by eight render files, two face-detection files, and audio runtime. Its 181 lines mix UI drag state, Vulkan handles, render packets, audio visualization, transcript state, and speaker overlays.
There are 29 render-runtime → Qt-editor-UI include edges. Extract a widget-free preview/render contract and leave interaction/transient UI state in a separate adapter.
Global debug configuration
[debug_controls.h (line 1)](/home/julian/Documents/JCut/debug_controls.h:1) has 42 include consumers across ten owners; its implementation is 975 lines. It is effectively a global configuration bus spanning UI, audio, decode, render, control-server, and face detection.
Split it into typed domain snapshots, with the control server and UI acting as adapters rather than every subsystem importing the global registry.
Editor window mega-header
[editor.h (line 1)](/home/julian/Documents/JCut/editor.h:1) is 915 lines, includes 31 repository headers, and is included by 19 implementation files. Internal implementation fragments should depend on narrower EditorWindow state/command interfaces instead of the complete application window.
I would address item 1 next because it is both the strongest centrality signal and the root of much of the remaining cross-owner coupling. The 35 reported layer violations are all L3→L2 edges, which ordinarily represent valid inward dependencies; the layer-direction policy should be corrected before treating those as refactoring findings.
The current viewer is available at [index.html](/home/julian/Documents/JCut/build/code-structure/index.html).
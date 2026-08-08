An immutable renderer-facing preview snapshot, with drag-only UI state kept out of the render contract.
The next preview-state cut should move remaining interaction/transient overrides out of that snapshot and into a smaller render-override packet.
A shared, headless Vulkan compositor used identically by preview and export.
Preview as an optional consumer that can drop display updates without ever blocking export.
Bounded semaphore/fence waits with explicit timeout diagnostics.
Swapchain presentation separated completely from render ownership.
Preview-window control calls marshaled onto the window thread so export and composition stay decoupled from UI affinity.
The next renderer boundary should be a narrow preview-window services interface so recording, resource ownership, and presentation bookkeeping do not depend on the full internal window header.
A long-running test covering preview closure/minimization, cancellation, checkpoint resume, and final ffprobe validation.

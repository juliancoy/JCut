A shared, headless Vulkan compositor used identically by preview and export.
Preview as an optional consumer that can drop display updates without ever blocking export.
Bounded semaphore/fence waits with explicit timeout diagnostics.
Swapchain presentation separated completely from render ownership.
A long-running test covering preview closure/minimization, cancellation, checkpoint resume, and final ffprobe validation.
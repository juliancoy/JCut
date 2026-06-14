Reduce decode superseding churn
Current profile shows 18925 superseded callbacks versus 12183 hardware completions. That means the decoder is doing useful GPU decode work, but too many requests become obsolete before presentation. Tighten request coalescing so playback dispatches only the newest needed visible frame plus a small forward window, and cancels/avoids older queued work earlier.

Make visible decode adaptive to playback speed
At 3x playback, visible_request_dispatch_rate is ~66/sec and exact hit rate drops to ~71%. The visible request window should scale by speed and decode latency, but cap duplicate in-flight requests per clip/frame range. Right now the retention policy reacts to latency, but dispatch still appears too eager.

Lower memory pressure from retained frame resources
RSS/PSS around 18-19 GB is too high. Audit frame/resource retention around decoded frame buffers, handoff resources, face artifacts, and cached timeline data. Keep the GPU preview cache bounded by bytes and time, not just frame count. Also add a profile bucket for retained decoded frames by owner so this is visible.

Separate “playback presentation” from “diagnostic/UI refresh” more strictly
The profile route already suppresses some JSON lookups during playback, which is good. Do the same for any nonessential per-frame UI table sync, overlay debug collection, frame traces, and diagnostics unless explicitly enabled. The playback path should update compact counters only.

Improve overlay/text prepare caching after the atlas refactor
Text atlas rebuilds are now separated, but layout prep still runs often: text_prep had 7833 attempts. Add a cheap key check before entering full text/layout prep, and cache speaker title/subtitle geometry per {clip, transcript section, active word, output rect}. Avoid building debug geometry unless requested.

Make exact-frame policy speed-aware
The app is presenting ~59 FPS, but at 3x the playhead advances faster than decode can always hit exactly. For high speeds, intentionally present nearest acceptable frames within a bounded lag instead of dispatching doomed exact requests. That should reduce latency spikes and superseded decode work.

Move track assignment post-update out of the hot UI path
One track assignment took 665 ms, mostly post-update. That should be queued/coalesced: mutate the model immediately, update the visible row immediately, then defer full refresh/save/recompute work.

Do a memory audit
Add a memory ownership profile
Before changing large retention code, add /profile counters for:
decoded frame cache bytes by clip
hardware frame handles retained
Vulkan handoff resources active/retired
transcript/face artifact cache bytes
timeline image/cache bytes
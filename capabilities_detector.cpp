#include "capabilities_detector.h"

#include <algorithm>

namespace {

std::string operatingSystemId() {
#if defined(__linux__)
  return "linux";
#elif defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "unknown";
#endif
}

} // namespace

RuntimeCapabilities detectRuntimeCapabilities() {
  return detectRuntimeCapabilities(false, {});
}

RuntimeCapabilities detectRuntimeCapabilities(
    bool sharedGpuRendererAvailable,
    const std::string& sharedGpuRendererStatus) {
  RuntimeCapabilities result;
  result.operatingSystem = operatingSystemId();
  result.videoDecodeBackends = jcut::detectedVideoDecodeCapabilities();

  AudioOutputBackendCapability pipewire;
  pipewire.kind = AudioOutputBackendKind::PipeWire;
  pipewire.id = "pipewire";
  pipewire.label = "Native PipeWire";
  pipewire.operatingSystem = result.operatingSystem;
#if defined(JCUT_HAVE_PIPEWIRE) && JCUT_HAVE_PIPEWIRE
  pipewire.compiled = true;
#endif
#if defined(__linux__)
  pipewire.operatingSystemSupported = true;
  pipewire.preference = 100;
  pipewire.reason = pipewire.compiled
                        ? "native nonblocking Linux audio graph"
                        : "PipeWire development support was unavailable at build time";
#else
  pipewire.reason = "PipeWire backend is Linux-only";
#endif
  result.audioOutputBackends.push_back(pipewire);

  AudioOutputBackendCapability rtaudio;
  rtaudio.kind = AudioOutputBackendKind::RtAudio;
  rtaudio.id = "rtaudio";
  rtaudio.label = "RtAudio";
  rtaudio.operatingSystem = result.operatingSystem;
  rtaudio.compiled = true;
  rtaudio.operatingSystemSupported = true;
#if defined(__linux__)
  rtaudio.preference = 50;
  rtaudio.reason = "portable fallback when native PipeWire cannot open an output";
#else
  rtaudio.preference = 100;
  rtaudio.reason = "preferred portable backend for this operating system";
#endif
  result.audioOutputBackends.push_back(rtaudio);

  RenderExportBackendCapability sharedGpu;
  sharedGpu.kind = RenderExportBackendKind::SharedGpu;
  sharedGpu.id = "shared_gpu";
  sharedGpu.label = "Shared GPU Renderer";
  sharedGpu.operatingSystem = result.operatingSystem;
  sharedGpu.compiled = true;
  sharedGpu.operatingSystemSupported = true;
  sharedGpu.preference = 100;
  sharedGpu.available = sharedGpuRendererAvailable;
  sharedGpu.reason = sharedGpuRendererAvailable
      ? "preferred accelerated export backend"
      : (sharedGpuRendererStatus.empty()
             ? "shared GPU renderer is unavailable"
             : sharedGpuRendererStatus);
  result.renderExportBackends.push_back(sharedGpu);

  RenderExportBackendCapability standaloneCpu;
  standaloneCpu.kind = RenderExportBackendKind::StandaloneCpu;
  standaloneCpu.id = "standalone_cpu";
  standaloneCpu.label = "Standalone CPU Renderer";
  standaloneCpu.operatingSystem = result.operatingSystem;
  standaloneCpu.compiled = true;
  standaloneCpu.operatingSystemSupported = true;
  standaloneCpu.preference = 50;
  standaloneCpu.available = true;
  standaloneCpu.reason =
      "portable fallback export backend when shared GPU export is unavailable";
  result.renderExportBackends.push_back(standaloneCpu);

  std::stable_sort(
      result.audioOutputBackends.begin(),
      result.audioOutputBackends.end(),
      [](const AudioOutputBackendCapability& left,
         const AudioOutputBackendCapability& right) {
        return left.preference > right.preference;
      });
  std::stable_sort(
      result.renderExportBackends.begin(),
      result.renderExportBackends.end(),
      [](const RenderExportBackendCapability& left,
         const RenderExportBackendCapability& right) {
        return left.preference > right.preference;
      });
  return result;
}

AudioOutputBackendSelection
selectBestAudioOutputBackend(const AudioOutputBackendConfig& config) {
  AudioOutputBackendSelection selection;
  const RuntimeCapabilities capabilities = detectRuntimeCapabilities();

  for (const AudioOutputBackendCapability& candidate :
       capabilities.audioOutputBackends) {
    AudioOutputBackendProbe probe;
    probe.capability = candidate;
    if (!candidate.compiled || !candidate.operatingSystemSupported) {
      probe.error = candidate.reason;
      selection.probes.push_back(std::move(probe));
      continue;
    }

    std::unique_ptr<AudioOutputBackend> backend =
        createAudioOutputBackend(candidate.kind);
    if (!backend) {
      probe.error = "backend factory unavailable";
      selection.probes.push_back(std::move(probe));
      continue;
    }
    if (!backend->initialize(config)) {
      probe.error = backend->lastError();
      backend->shutdown();
      selection.probes.push_back(std::move(probe));
      continue;
    }

    probe.available = true;
    probe.info = backend->info();
    selection.info = probe.info;
    selection.selectedId = candidate.id;
    selection.selectionReason = candidate.reason;
    selection.probes.push_back(std::move(probe));
    selection.backend = std::move(backend);
    break;
  }
  return selection;
}

const RenderExportBackendCapability*
selectPreferredRenderExportBackend(
    const RuntimeCapabilities& capabilities) {
  for (const RenderExportBackendCapability& capability :
       capabilities.renderExportBackends) {
    if (capability.compiled &&
        capability.operatingSystemSupported &&
        capability.available) {
      return &capability;
    }
  }
  return capabilities.renderExportBackends.empty()
      ? nullptr
      : &capabilities.renderExportBackends.front();
}

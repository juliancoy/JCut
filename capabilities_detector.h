#pragma once

#include "audio_output_backend.h"
#include "video_decode_capabilities_core.h"

#include <string>
#include <vector>

struct AudioOutputBackendCapability {
  AudioOutputBackendKind kind = AudioOutputBackendKind::RtAudio;
  std::string id;
  std::string label;
  std::string operatingSystem;
  int preference = 0;
  bool compiled = false;
  bool operatingSystemSupported = false;
  std::string reason;
};

enum class RenderExportBackendKind {
  SharedGpu,
  StandaloneCpu,
};

struct RenderExportBackendCapability {
  RenderExportBackendKind kind = RenderExportBackendKind::StandaloneCpu;
  std::string id;
  std::string label;
  std::string operatingSystem;
  int preference = 0;
  bool compiled = false;
  bool operatingSystemSupported = false;
  bool available = false;
  std::string reason;
};

struct RuntimeCapabilities {
  std::string operatingSystem;
  std::vector<AudioOutputBackendCapability> audioOutputBackends;
  std::vector<jcut::VideoDecodeBackendCapability> videoDecodeBackends;
  std::vector<RenderExportBackendCapability> renderExportBackends;
};

struct AudioOutputBackendProbe {
  AudioOutputBackendCapability capability;
  bool available = false;
  AudioOutputBackendInfo info;
  std::string error;
};

struct AudioOutputBackendSelection {
  std::unique_ptr<AudioOutputBackend> backend;
  AudioOutputBackendInfo info;
  std::vector<AudioOutputBackendProbe> probes;
  std::string selectedId;
  std::string selectionReason;
};

RuntimeCapabilities detectRuntimeCapabilities();
RuntimeCapabilities detectRuntimeCapabilities(
    bool sharedGpuRendererAvailable,
    const std::string& sharedGpuRendererStatus = {});
AudioOutputBackendSelection
selectBestAudioOutputBackend(const AudioOutputBackendConfig& config);
const RenderExportBackendCapability*
selectPreferredRenderExportBackend(
    const RuntimeCapabilities& capabilities);

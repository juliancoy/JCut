#pragma once

#include "audio_output_backend.h"

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

struct RuntimeCapabilities {
  std::string operatingSystem;
  std::vector<AudioOutputBackendCapability> audioOutputBackends;
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
AudioOutputBackendSelection
selectBestAudioOutputBackend(const AudioOutputBackendConfig& config);

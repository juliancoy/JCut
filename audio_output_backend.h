#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

using AudioOutputCallback =
    int (*)(void* outputBuffer,
            void* inputBuffer,
            unsigned int frameCount,
            double streamTime,
            unsigned int status,
            void* userData);

struct AudioOutputBackendConfig {
  int sampleRate = 48000;
  int channelCount = 2;
  int requestedPeriodFrames = 1024;
  AudioOutputCallback callback = nullptr;
  void* userData = nullptr;
};

struct AudioOutputBackendInfo {
  std::string apiName;
  std::string deviceName;
  int deviceCount = 0;
  int defaultDeviceId = 0;
  int outputChannels = 0;
  int periodFrames = 0;
  int64_t latencyFrames = 0;
  bool defaultDeviceValid = false;
};

enum class AudioOutputBackendKind {
  PipeWire,
  RtAudio,
};

class AudioOutputBackend {
public:
  virtual ~AudioOutputBackend() = default;

  virtual bool initialize(const AudioOutputBackendConfig& config) = 0;
  virtual void shutdown() = 0;

  // These requests must not wait for the real-time device callback.
  virtual bool start() = 0;
  virtual bool stop(bool drain) = 0;

  virtual bool isOpen() const = 0;
  virtual bool isRunning() const = 0;
  virtual bool supportsSeamlessReprime() const = 0;
  virtual int64_t latencyFrames() const = 0;
  virtual uint64_t connectionRevision() const = 0;
  virtual AudioOutputBackendInfo info() const = 0;
  virtual std::string lastError() const = 0;
};

std::unique_ptr<AudioOutputBackend>
createAudioOutputBackend(AudioOutputBackendKind kind);

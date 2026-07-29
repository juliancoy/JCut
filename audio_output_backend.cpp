#include "audio_output_backend.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(JCUT_HAVE_PIPEWIRE) && JCUT_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

namespace {

class PipeWireAudioOutputBackend final : public AudioOutputBackend {
public:
  ~PipeWireAudioOutputBackend() override { shutdown(); }

  bool initialize(const AudioOutputBackendConfig& config) override {
    if (!config.callback || config.sampleRate <= 0 ||
        config.channelCount <= 0 || config.channelCount > 2) {
      setError("invalid PipeWire output configuration");
      return false;
    }

    std::unique_lock<std::mutex> lock(m_controlMutex);
    if (m_worker.joinable()) {
      return m_open.load(std::memory_order_acquire);
    }
    m_config = config;
    m_shutdownRequested = false;
    m_desiredActive = false;
    m_reconnectRequested = true;
    m_worker = std::thread([this]() { controlLoop(); });
    m_controlCondition.wait_for(lock, std::chrono::seconds(4), [this]() {
      return m_open.load(std::memory_order_acquire) ||
             m_shutdownRequested;
    });
    return m_open.load(std::memory_order_acquire);
  }

  void shutdown() override {
    {
      std::lock_guard<std::mutex> lock(m_controlMutex);
      if (!m_worker.joinable()) {
        return;
      }
      m_shutdownRequested = true;
      m_desiredActive = false;
      m_controlCondition.notify_all();
    }
    m_worker.join();
  }

  bool start() override {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if (!m_worker.joinable()) {
      return false;
    }
    m_desiredActive = true;
    if (!m_open.load(std::memory_order_acquire)) {
      m_reconnectRequested = true;
    }
    m_controlCondition.notify_all();
    return true;
  }

  bool stop(bool /*drain*/) override {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if (!m_worker.joinable()) {
      return false;
    }
    m_desiredActive = false;
    m_controlCondition.notify_all();
    return true;
  }

  bool isOpen() const override {
    return m_open.load(std::memory_order_acquire);
  }

  bool isRunning() const override {
    return m_running.load(std::memory_order_acquire);
  }

  bool supportsSeamlessReprime() const override { return true; }

  int64_t latencyFrames() const override {
    return m_latencyFrames.load(std::memory_order_acquire);
  }

  uint64_t connectionRevision() const override {
    return m_connectionRevision.load(std::memory_order_acquire);
  }

  AudioOutputBackendInfo info() const override {
    AudioOutputBackendInfo result;
    result.apiName = "pipewire";
    result.deviceName = "PipeWire default output";
    result.deviceCount = isOpen() ? 1 : 0;
    result.defaultDeviceId = 0;
    result.outputChannels = m_config.channelCount;
    result.periodFrames =
        m_periodFrames.load(std::memory_order_acquire);
    result.latencyFrames = latencyFrames();
    result.defaultDeviceValid = isOpen();
    return result;
  }

  std::string lastError() const override {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
  }

private:
  static void onStateChanged(void* data,
                             enum pw_stream_state /*oldState*/,
                             enum pw_stream_state state,
                             const char* error) {
    auto* self = static_cast<PipeWireAudioOutputBackend*>(data);
    self->handleStateChanged(state, error);
  }

  static void onProcess(void* data) {
    static_cast<PipeWireAudioOutputBackend*>(data)->process();
  }

  void handleStateChanged(enum pw_stream_state state, const char* error) {
    switch (state) {
    case PW_STREAM_STATE_PAUSED:
      m_open.store(true, std::memory_order_release);
      m_running.store(false, std::memory_order_release);
      clearError();
      break;
    case PW_STREAM_STATE_STREAMING:
      m_open.store(true, std::memory_order_release);
      m_running.store(true, std::memory_order_release);
      m_connectionRevision.fetch_add(1, std::memory_order_acq_rel);
      m_lastProcessNs.store(monotonicNowNs(), std::memory_order_release);
      clearError();
      break;
    case PW_STREAM_STATE_ERROR:
    case PW_STREAM_STATE_UNCONNECTED:
      m_open.store(false, std::memory_order_release);
      m_running.store(false, std::memory_order_release);
      if (state == PW_STREAM_STATE_ERROR) {
        setError(error && *error ? error : "PipeWire stream error");
      }
      requestReconnect();
      break;
    case PW_STREAM_STATE_CONNECTING:
      m_open.store(false, std::memory_order_release);
      m_running.store(false, std::memory_order_release);
      break;
    }
    m_controlCondition.notify_all();
  }

  void process() {
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(m_stream);
    if (!pipewireBuffer) {
      return;
    }
    spa_buffer* buffer = pipewireBuffer->buffer;
    spa_data* data = buffer && buffer->n_datas > 0
                         ? &buffer->datas[0]
                         : nullptr;
    if (!data || !data->data || !data->chunk) {
      pw_stream_queue_buffer(m_stream, pipewireBuffer);
      return;
    }

    const uint32_t stride =
        static_cast<uint32_t>(m_config.channelCount * sizeof(int16_t));
    uint32_t frameCount = stride > 0 ? data->maxsize / stride : 0;
    if (pipewireBuffer->requested > 0) {
      frameCount =
          std::min<uint64_t>(frameCount, pipewireBuffer->requested);
    }
    if (frameCount == 0) {
      data->chunk->offset = 0;
      data->chunk->stride = static_cast<int32_t>(stride);
      data->chunk->size = 0;
      pw_stream_queue_buffer(m_stream, pipewireBuffer);
      return;
    }

    std::memset(data->data, 0,
                static_cast<size_t>(frameCount) * stride);
    m_config.callback(data->data,
                      nullptr,
                      frameCount,
                      0.0,
                      0,
                      m_config.userData);
    data->chunk->offset = 0;
    data->chunk->stride = static_cast<int32_t>(stride);
    data->chunk->size = frameCount * stride;
    m_periodFrames.store(static_cast<int>(frameCount),
                         std::memory_order_release);
    m_latencyFrames.store(static_cast<int64_t>(frameCount),
                          std::memory_order_release);
    m_lastProcessNs.store(monotonicNowNs(), std::memory_order_release);
    pw_stream_queue_buffer(m_stream, pipewireBuffer);
  }

  void controlLoop() {
    pw_init(nullptr, nullptr);
    int retryDelayMs = 100;
    bool appliedActive = false;
    int64_t activeRequestedNs = 0;

    while (true) {
      bool shouldReconnect = false;
      bool desiredActive = false;
      {
        std::unique_lock<std::mutex> lock(m_controlMutex);
        m_controlCondition.wait_for(
            lock, std::chrono::milliseconds(200), [this, appliedActive]() {
              return m_shutdownRequested || m_reconnectRequested ||
                     m_desiredActive != appliedActive;
            });
        if (m_shutdownRequested) {
          break;
        }
        desiredActive = m_desiredActive;
        shouldReconnect = m_reconnectRequested || !m_stream;
        m_reconnectRequested = false;
      }

      if (m_running.load(std::memory_order_acquire) && desiredActive) {
        const int64_t lastProcess =
            m_lastProcessNs.load(std::memory_order_acquire);
        if (lastProcess > 0 &&
            monotonicNowNs() - lastProcess >
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::seconds(2)).count()) {
          setError("PipeWire output callback stalled; reconnecting");
          shouldReconnect = true;
        }
      }
      if (desiredActive && appliedActive &&
          !m_running.load(std::memory_order_acquire) &&
          activeRequestedNs > 0 &&
          monotonicNowNs() - activeRequestedNs >
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::seconds(2)).count()) {
        setError("PipeWire output did not resume; reconnecting");
        shouldReconnect = true;
      }

      if (shouldReconnect) {
        destroyPipeline();
        appliedActive = false;
        activeRequestedNs = 0;
        if (!createPipeline()) {
          std::unique_lock<std::mutex> lock(m_controlMutex);
          m_controlCondition.wait_for(
              lock, std::chrono::milliseconds(retryDelayMs), [this]() {
                return m_shutdownRequested || m_reconnectRequested;
              });
          retryDelayMs = std::min(5000, retryDelayMs * 2);
          continue;
        }
        retryDelayMs = 100;
      }

      if (m_stream && desiredActive != appliedActive) {
        pw_thread_loop_lock(m_threadLoop);
        const int result =
            pw_stream_set_active(m_stream, desiredActive);
        pw_thread_loop_unlock(m_threadLoop);
        if (result < 0) {
          setError(std::string("PipeWire set-active failed: ") +
                   spa_strerror(result));
          requestReconnect();
        } else {
          appliedActive = desiredActive;
          activeRequestedNs =
              desiredActive ? monotonicNowNs() : 0;
          if (!desiredActive) {
            m_running.store(false, std::memory_order_release);
          }
        }
      }
    }

    destroyPipeline();
    pw_deinit();
    {
      std::lock_guard<std::mutex> lock(m_controlMutex);
      m_shutdownRequested = false;
    }
  }

  bool createPipeline() {
    m_threadLoop =
        pw_thread_loop_new("jcut-pipewire-output", nullptr);
    if (!m_threadLoop) {
      setError("unable to create PipeWire thread loop");
      return false;
    }
    m_context =
        pw_context_new(pw_thread_loop_get_loop(m_threadLoop), nullptr, 0);
    if (!m_context) {
      setError("unable to create PipeWire context");
      destroyPipeline();
      return false;
    }
    m_core = pw_context_connect(m_context, nullptr, 0);
    if (!m_core) {
      setError("unable to connect to PipeWire");
      destroyPipeline();
      return false;
    }

    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_NODE_NAME, "jcut-native-output",
        PW_KEY_NODE_DESCRIPTION, "JCut Audio Output",
        nullptr);
    m_stream = pw_stream_new(m_core, "JCut Audio Output", properties);
    if (!m_stream) {
      setError("unable to create PipeWire output stream");
      destroyPipeline();
      return false;
    }

    static const pw_stream_events events = []() {
      pw_stream_events value{};
      value.version = PW_VERSION_STREAM_EVENTS;
      value.state_changed =
          &PipeWireAudioOutputBackend::onStateChanged;
      value.process = &PipeWireAudioOutputBackend::onProcess;
      return value;
    }();
    pw_stream_add_listener(m_stream, &m_streamListener, &events, this);

    uint8_t podBuffer[1024];
    spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16;
    format.rate = static_cast<uint32_t>(m_config.sampleRate);
    format.channels = static_cast<uint32_t>(m_config.channelCount);
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    if (m_config.channelCount > 1) {
      format.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const spa_pod* params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};

    const int connectResult = pw_stream_connect(
        m_stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS |
            PW_STREAM_FLAG_INACTIVE),
        params,
        1);
    if (connectResult < 0) {
      setError(std::string("unable to connect PipeWire stream: ") +
               spa_strerror(connectResult));
      destroyPipeline();
      return false;
    }
    if (pw_thread_loop_start(m_threadLoop) < 0) {
      setError("unable to start PipeWire thread loop");
      destroyPipeline();
      return false;
    }
    m_threadLoopStarted = true;

    std::unique_lock<std::mutex> lock(m_controlMutex);
    m_controlCondition.wait_for(lock, std::chrono::seconds(3), [this]() {
      return m_open.load(std::memory_order_acquire) ||
             m_shutdownRequested;
    });
    if (!m_open.load(std::memory_order_acquire)) {
      setError("timed out opening PipeWire output stream");
      lock.unlock();
      destroyPipeline();
      return false;
    }
    clearError();
    return true;
  }

  void destroyPipeline() {
    m_open.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_threadLoopStarted && m_threadLoop) {
      pw_thread_loop_stop(m_threadLoop);
      m_threadLoopStarted = false;
    }
    if (m_stream) {
      spa_hook_remove(&m_streamListener);
      pw_stream_destroy(m_stream);
      m_stream = nullptr;
    }
    if (m_core) {
      pw_core_disconnect(m_core);
      m_core = nullptr;
    }
    if (m_context) {
      pw_context_destroy(m_context);
      m_context = nullptr;
    }
    if (m_threadLoop) {
      pw_thread_loop_destroy(m_threadLoop);
      m_threadLoop = nullptr;
    }
  }

  void requestReconnect() {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if (!m_shutdownRequested) {
      m_reconnectRequested = true;
      m_controlCondition.notify_all();
    }
  }

  void setError(std::string error) {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(error);
  }

  void clearError() {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();
  }

  static int64_t monotonicNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  AudioOutputBackendConfig m_config;
  std::thread m_worker;
  mutable std::mutex m_controlMutex;
  std::condition_variable m_controlCondition;
  bool m_shutdownRequested = false;
  bool m_desiredActive = false;
  bool m_reconnectRequested = false;

  mutable std::mutex m_errorMutex;
  std::string m_lastError;

  pw_thread_loop* m_threadLoop = nullptr;
  pw_context* m_context = nullptr;
  pw_core* m_core = nullptr;
  pw_stream* m_stream = nullptr;
  spa_hook m_streamListener{};
  bool m_threadLoopStarted = false;

  std::atomic<bool> m_open{false};
  std::atomic<bool> m_running{false};
  std::atomic<int> m_periodFrames{1024};
  std::atomic<int64_t> m_latencyFrames{0};
  std::atomic<int64_t> m_lastProcessNs{0};
  std::atomic<uint64_t> m_connectionRevision{0};
};

} // namespace

#endif

#include <RtAudio.h>

namespace {

class RtAudioOutputBackend final : public AudioOutputBackend {
public:
  ~RtAudioOutputBackend() override { shutdown(); }

  bool initialize(const AudioOutputBackendConfig& config) override {
    m_config = config;
    try {
      m_audio = std::make_unique<rt::audio::RtAudio>();
      m_audio->showWarnings(false);
      const unsigned int count = m_audio->getDeviceCount();
      if (count == 0) {
        m_lastError = "no audio output devices found";
        m_audio.reset();
        return false;
      }
      rt::audio::RtAudio::StreamParameters params;
      params.deviceId = m_audio->getDefaultOutputDevice();
      params.nChannels = static_cast<unsigned int>(config.channelCount);
      const auto device = m_audio->getDeviceInfo(params.deviceId);
      unsigned int frames =
          static_cast<unsigned int>(config.requestedPeriodFrames);
      const auto error = m_audio->openStream(
          &params, nullptr, rt::audio::RTAUDIO_SINT16,
          static_cast<unsigned int>(config.sampleRate), &frames,
          config.callback, config.userData);
      if (error != rt::audio::RTAUDIO_NO_ERROR) {
        m_lastError = m_audio->getErrorText();
        m_audio.reset();
        return false;
      }
      m_info.apiName =
          rt::audio::RtAudio::getApiName(m_audio->getCurrentApi());
      m_info.deviceName = device.name;
      m_info.deviceCount = static_cast<int>(count);
      m_info.defaultDeviceId = static_cast<int>(params.deviceId);
      m_info.outputChannels = static_cast<int>(device.outputChannels);
      m_info.periodFrames = static_cast<int>(frames);
      m_info.latencyFrames = m_audio->getStreamLatency();
      m_info.defaultDeviceValid = true;
      m_lastError.clear();
      return true;
    } catch (const std::exception& error) {
      m_lastError = error.what();
      m_audio.reset();
      return false;
    }
  }

  void shutdown() override {
    if (!m_audio) {
      return;
    }
    if (m_audio->isStreamRunning()) {
      m_audio->stopStream();
    }
    if (m_audio->isStreamOpen()) {
      m_audio->closeStream();
    }
    m_audio.reset();
  }

  bool start() override {
    if (!m_audio) {
      return false;
    }
    const auto result = m_audio->startStream();
    if (result != rt::audio::RTAUDIO_NO_ERROR) {
      m_lastError = m_audio->getErrorText();
      return false;
    }
    m_connectionRevision.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  bool stop(bool drain) override {
    if (!m_audio || !m_audio->isStreamRunning()) {
      return true;
    }
    const auto result =
        drain ? m_audio->stopStream() : m_audio->abortStream();
    if (result != rt::audio::RTAUDIO_NO_ERROR) {
      m_lastError = m_audio->getErrorText();
      return false;
    }
    return true;
  }

  bool isOpen() const override {
    return m_audio && m_audio->isStreamOpen();
  }

  bool isRunning() const override {
    return m_audio && m_audio->isStreamRunning();
  }

  bool supportsSeamlessReprime() const override { return false; }

  int64_t latencyFrames() const override {
    return m_audio && m_audio->isStreamOpen()
               ? m_audio->getStreamLatency()
               : 0;
  }

  uint64_t connectionRevision() const override {
    return m_connectionRevision.load(std::memory_order_acquire);
  }

  AudioOutputBackendInfo info() const override { return m_info; }

  std::string lastError() const override { return m_lastError; }

private:
  AudioOutputBackendConfig m_config;
  std::unique_ptr<rt::audio::RtAudio> m_audio;
  AudioOutputBackendInfo m_info;
  std::string m_lastError;
  std::atomic<uint64_t> m_connectionRevision{0};
};

} // namespace

std::unique_ptr<AudioOutputBackend>
createAudioOutputBackend(AudioOutputBackendKind kind) {
  switch (kind) {
  case AudioOutputBackendKind::PipeWire:
#if defined(JCUT_HAVE_PIPEWIRE) && JCUT_HAVE_PIPEWIRE
    return std::make_unique<PipeWireAudioOutputBackend>();
#else
    return {};
#endif
  case AudioOutputBackendKind::RtAudio:
    return std::make_unique<RtAudioOutputBackend>();
  }
  return {};
}

#pragma once

#include "timeline_fps.h"

#include <cstdint>

constexpr int kAudioSampleRate = 48000;
constexpr int64_t kSamplesPerFrame = kAudioSampleRate / kTimelineFps;
constexpr int64_t kAudioNudgeSamples = (kAudioSampleRate * 25) / 1000;

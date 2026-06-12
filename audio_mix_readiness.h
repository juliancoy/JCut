#pragma once

#include <cstdint>

// Multi-stream mix readiness policy. Source of truth for the rules in
// TIME.md "Multi-Stream Audio Readiness".
//
// A starved clip (audio cache does not cover the requested mix range) is
// dropped from the chunk and decoded in the background; it must not stall
// clips that are ready. Blocking the chunk — and with it the audio master
// clock — is allowed only when no active clip can contribute, because
// emitting silence over content that merely is not decoded yet would skip
// that content instead of waiting for it.

// Prepare-stage rule: block the chunk only when clips wanted audio and none
// of them produced a usable cache entry.
inline bool mixPrepareMustBlock(int preparedClipCount, int cacheMissCount,
                                int invalidAudioCount) {
  return preparedClipCount == 0 &&
         (cacheMissCount > 0 || invalidAudioCount > 0);
}

// Frame-emission rule: a frame may be emitted while clips are starved only
// if at least one active clip contributed in-range audio at that frame.
inline bool mixFrameMustBlock(bool frameHadActiveClip,
                              bool frameHadReadyContribution,
                              bool frameHadStarvedClip) {
  return frameHadActiveClip && !frameHadReadyContribution &&
         frameHadStarvedClip;
}

// Splice rule: a speech-range crossfade secondary tap may pull audio only
// from a clip whose timeline extent contains the secondary timeline sample.
// Outside its extent a clip has no audio at any gain.
inline bool spliceSecondaryTapWithinClip(int64_t secondaryTimelineSample,
                                         int64_t clipStartSample,
                                         int64_t clipEndSampleExclusive) {
  return secondaryTimelineSample >= clipStartSample &&
         secondaryTimelineSample < clipEndSampleExclusive;
}

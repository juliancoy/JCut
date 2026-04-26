# Audio Preview And Dynamics

## Preview Modes
- `Video`: normal composited visual preview.
- `Audio`: waveform-focused preview mode.

`feature_audio_preview_mode` controls whether `Audio` mode is available.

## Dynamics Controls
Runtime controls are non-destructive and currently include:
- Normalize (target dB)
- Peak reduction (threshold)
- Limiter (threshold)
- Compressor (threshold + ratio)

`feature_audio_dynamics_tools` controls availability of the FX dialog.

## Signal Flow (Current)
Waveform decode -> optional dynamics shaping (preview path) -> render overlay.

## Persistence
The following values persist per project:
- preview mode
- normalize enabled/target
- peak reduction enabled/threshold
- limiter enabled/threshold
- compressor enabled/threshold/ratio

## Future Hardening
- Add LUFS/true-peak metering and calibration.
- Add explicit chain-order editor and bypass per stage.
- Add multiresolution waveform cache bounds in settings.

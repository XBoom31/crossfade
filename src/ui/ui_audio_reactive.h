#pragma once

// Lightweight, real-time audio analysis for the UI.
//
// Captures the default sink's monitor (i.e. what the app is actually playing)
// through `parec`, so the visuals follow the music without touching the
// player pipeline. All values are smoothed, normalised 0..1 and safe to read
// from the render thread while the capture thread keeps writing.
struct AudioReactive
{
    float rms = 0.0f;    // overall loudness
    float bass = 0.0f;   // < ~160 Hz
    float mid = 0.0f;    // ~160 Hz .. 2 kHz
    float treble = 0.0f; // > ~2 kHz
    float instrument = 0.0f; // ~320 Hz .. 2.5 kHz: guitar/keys/vocal body
    float density = 0.0f;    // zero-crossing density: how "busy" the waveform is
    float beat = 0.0f;       // decaying impulse on bass transients
    float pluck = 0.0f;      // decaying impulse on instrument-band onsets
    bool active = false;     // capture running and delivering samples
};

// Start/stop the capture thread. Safe to call repeatedly; start() is a no-op
// while it is already running and stop() is a no-op while it is not.
void ui_audio_start();
void ui_audio_stop();

const AudioReactive &ui_audio_levels();

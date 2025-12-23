#include "SynthVoice.h"

// Simplified Trigger (Uses stored settings)
void SynthVoice::trigger(float freq, int noteIdx) {
  trigger(freq, noteIdx, waveform, pulseWidth, attackTime, decayTime,
          sustainLvl, releaseTime);
}

// Full Trigger
void SynthVoice::trigger(float freq, int noteIdx, Waveform wave, float pw,
                         float attack, float decay, float sustain,
                         float release) {
  if (freq < 1.0f)
    return;

  active = true;
  held = true;
  noteIndex = noteIdx;
  frequency = freq;
  phase = 0.0f;
  phaseIncrement = freq / (float)activeSampleRate;
  waveform = wave;
  pulseWidth = pw;

  // Envelope Setup
  envState = ENV_ATTACK;
  envelope = 0.0f;

  if (attack < 0.001f)
    attack = 0.001f;
  if (decay < 0.001f)
    decay = 0.001f;
  if (release < 0.001f)
    release = 0.001f;

  attackRate = 1.0f / (attack * (float)activeSampleRate);
  decayRate = 1.0f / (decay * (float)activeSampleRate);
  releaseRate = 1.0f / (release * (float)activeSampleRate);
  sustainLvl = sustain;
}

void SynthVoice::release() {
  if (!active)
    return;
  held = false;
  envState = ENV_RELEASE;
}

// Generate one sample
float IRAM_ATTR SynthVoice::getSample(float pitchMod, float pwMod) {
  if (!active)
    return 0;

  float currentInc = phaseIncrement * pitchMod;

  phase += currentInc;
  if (phase >= 1.0f)
    phase -= 1.0f;

  float sample = 0.0f;

  // Anti-Aliasing Strategy: Force Sine for Sparkle (High Pitch)
  // Soft Blend for high frequency: Smooth Saw/Square -> Triangle at > 2.5kHz
  Waveform effWave = isSparkle ? WAVE_SINE : waveform;
  float freq = phaseIncrement * (float)activeSampleRate;
  float blend = 0.0f;
  if (!isSparkle && (effWave == WAVE_SAW || effWave == WAVE_SQUARE)) {
    if (freq > 2500.0f) {
      blend = (freq - 2500.0f) / 1000.0f; // 2.5k to 3.5k transition
      if (blend > 1.0f)
        blend = 1.0f;
    }
  }

  if (effWave == WAVE_SAW) {
    float raw = (phase * 2.0f) - 1.0f;
    if (pwMod > 0.01f) {
      float gain = 1.0f + (pwMod * 3.0f);
      raw *= gain;
      if (raw > 1.0f)
        raw = 2.0f - raw;
      if (raw < -1.0f)
        raw = -2.0f - raw;
      if (raw > 1.0f)
        raw = 2.0f - raw;
      if (raw < -1.0f)
        raw = -2.0f - raw;
    }
    sample = raw;

    if (blend > 0.01f) {
      float t = (phase * 2.0f) - 1.0f;
      float tri = 2.0f * fabs(t) - 1.0f;
      sample = sample * (1.0f - blend) + tri * blend;
    }
  } else if (effWave == WAVE_SQUARE) {
    float pw = 0.5f + pwMod;
    if (pw < 0.05f)
      pw = 0.05f;
    if (pw > 0.95f)
      pw = 0.95f;
    float raw = (phase < pw) ? 1.0f : -1.0f;
    float dcOffset = 2.0f * pw - 1.0f;
    sample = raw - dcOffset;

    if (blend > 0.01f) {
      float t = (phase * 2.0f) - 1.0f;
      float tri = 2.0f * fabs(t) - 1.0f;
      sample = sample * (1.0f - blend) + tri * blend;
    }
  } else if (effWave == WAVE_SINE) {
    // High quality sine via LUT with Linear Interpolation
    // Phase 0..1 maps to 0..256
    float p = phase * 256.0f;
    int idx = (int)p;
    float frac = p - idx;

    // Wrap index safely
    int idx2 = (idx + 1) & 0xFF; // 256 wrap
    idx &= 0xFF;

    float s1 = sineLUT[idx];
    float s2 = sineLUT[idx2];

    sample = s1 + (s2 - s1) * frac;
  } else if (effWave == WAVE_TRIANGLE) {
    // Triangle with Wavefolding
    float t = (phase * 2.0f) - 1.0f;
    float tri = 2.0f * fabs(t) - 1.0f;

    float gain = 1.0f + (pulseWidth * 5.0f);
    tri *= gain;

    // Safer Fold
    if (tri > 1.0f)
      tri = 2.0f - tri;
    if (tri < -1.0f)
      tri = -2.0f - tri;
    if (tri > 1.0f)
      tri = 2.0f - tri;
    if (tri < -1.0f)
      tri = -2.0f - tri;

    sample = tri;
  }

  // Apply Gain
  sample *= mixGain;

  // 2. Apply Envelope
  switch (envState) {
  case ENV_ATTACK:
    envelope += attackRate;
    if (envelope >= 1.0f) {
      envelope = 1.0f;
      if (held)
        envState = ENV_DECAY;
      else
        envState = ENV_RELEASE;
    }
    break;

  case ENV_DECAY:
    envelope -= decayRate;
    if (envelope <= sustainLvl) {
      envelope = sustainLvl;
      envState = ENV_SUSTAIN;
    }
    break;

  case ENV_SUSTAIN:
    if (!held)
      envState = ENV_RELEASE;
    break;

  case ENV_RELEASE:
    envelope -= releaseRate;
    if (envelope <= 0.0f) {
      envelope = 0.0f;
      active = false;
      envState = ENV_IDLE;
    }
    break;

  case ENV_IDLE:
    active = false;
    break;
  }

  return sample * envelope;
}

// --- Static Resources ---
float SynthVoice::sineLUT[256];

void SynthVoice::initLUT() {
  for (int i = 0; i < 256; i++) {
    sineLUT[i] = sin((float)i * 6.283185307f / 256.0f);
  }
}

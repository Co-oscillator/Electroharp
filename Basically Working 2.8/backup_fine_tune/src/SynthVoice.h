#ifndef SYNTH_VOICE_H
#define SYNTH_VOICE_H

#include "Config.h"
#include <Arduino.h>

enum Waveform { WAVE_SAW, WAVE_SQUARE, WAVE_SINE, WAVE_TRIANGLE };
enum EnvState { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

class SynthVoice {
public:
  bool active = false;
  bool held = false;
  int noteIndex = -1;

  float frequency;
  float phase;
  float phaseIncrement;
  float pulseWidth = 0.5f;

  Waveform waveform = WAVE_SAW; // Default

  // ADSR State
  float envelope = 0.0f;
  EnvState envState = ENV_IDLE;

  // ADSR Settings (Stored)
  float attackTime = 0.01f;
  float decayTime = 0.1f;
  float sustainLvl = 0.7f;
  float releaseTime = 0.3f;

  // Calculated Rates
  float attackRate;
  float decayRate;
  float releaseRate;

  // Setters
  void setWaveform(Waveform w) { waveform = w; }
  void setPulseWidth(float pw) { pulseWidth = pw; }
  void setADSR(float a, float d, float s, float r) {
    attackTime = a;
    decayTime = d;
    sustainLvl = s;
    releaseTime = r;
    // Pre-calc rates not needed here if calculated in trigger,
    // but good for optimization if changed rarely.
    // For now, calc in trigger to be safe or calc here?
    // Let's calc in trigger to ensure sample rate is respected at runtime ?
    // No, sample rate is constant. Calc here is efficient.
  }

  // Simplified Trigger (Uses stored settings)
  void trigger(float freq, int noteIdx) {
    trigger(freq, noteIdx, waveform, pulseWidth, attackTime, decayTime,
            sustainLvl, releaseTime);
  }

  // Full Trigger
  void trigger(float freq, int noteIdx, Waveform wave, float pw, float attack,
               float decay, float sustain, float release) {
    if (freq < 1.0f)
      return;

    active = true;
    held = true;
    noteIndex = noteIdx;
    frequency = freq;
    phase = 0.0f;
    phaseIncrement = freq / (float)SAMPLE_RATE;
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

    attackRate = 1.0f / (attack * SAMPLE_RATE);
    decayRate = 1.0f / (decay * SAMPLE_RATE);
    releaseRate = 1.0f / (release * SAMPLE_RATE);
    sustainLvl = sustain;
  }

  void release() {
    if (!active)
      return;
    held = false;
    envState = ENV_RELEASE;
  }

  // Generate one sample
  float IRAM_ATTR getSample(float pitchMod) {
    if (!active)
      return 0;

    float currentInc = phaseIncrement;
    if (waveform == WAVE_SINE) {
      currentInc *= pitchMod;
    }

    phase += currentInc;
    if (phase >= 1.0f)
      phase -= 1.0f;

    float sample = 0.0f;

    if (waveform == WAVE_SAW) {
      sample = (phase * 2.0f) - 1.0f; // -1 to 1
      sample *= 0.60f;                // Reduced volume (User req 90% then 80%)
    } else if (waveform == WAVE_SQUARE) {
      // Naive PWM with DC Correction
      float raw = (phase < pulseWidth) ? 1.0f : -1.0f;
      float dcOffset = 2.0f * pulseWidth - 1.0f;
      sample = raw - dcOffset;
      sample *= 0.5f; // Reduce Square gain
    } else if (waveform == WAVE_SINE) {
      // High quality sine (No Wavefolding)
      sample = sin(phase * 6.283185307f);
      sample *= 0.82f; // Boosted volume (0.65 -> 0.82)
    } else if (waveform == WAVE_TRIANGLE) {
      // Triangle with Wavefolding
      float t = (phase * 2.0f) - 1.0f;
      float tri = 2.0f * fabs(t) - 1.0f;

      float gain = 1.0f + (pulseWidth * 5.0f);
      tri *= gain;

      while (tri > 1.0f)
        tri = 2.0f - tri;
      while (tri < -1.0f)
        tri = -2.0f - tri;

      sample = tri;
      sample *= 0.30f;
    }

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
};

#endif

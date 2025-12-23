// --- ESP32 CYD Autoharp - Fixed Setup Duplicate ---
#include "Config.h"
#include "SynthVoice.h"
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// --- AUDIO CONFIG ---
// Both Included for Dynamic Switching
#include "BluetoothA2DPSource.h"
#include "soc/rtc_io_reg.h"
#include "soc/sens_reg.h"
#include <driver/dac.h>

BluetoothA2DPSource a2dp_source;

// Wired Audio Globals
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Boot State
enum AudioTarget {
  TARGET_BOOT,
  TARGET_SPEAKER,
  TARGET_BLUETOOTH,
  TARGET_BT_SELECT
};
AudioTarget audioTarget = TARGET_BOOT;

// --- Globals ---
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

SynthVoice voices[MAX_VOICES];
float baseFreqs[STRING_COUNT];
int octaveShift = 0;
uint16_t activeChord = 0;
uint16_t currentChordMask = 0xFFFF;
int activeButtonIndex = -1;

// --- Sound Profiles ---
// Bluetooth: High Fidelity, Balanced, Lush
// Bluetooth: High Fidelity, Balanced, Lush
SoundProfile btProfile = {
    0.90f, // Saw
    0.50f, // Square
    0.40f, // Sine (Deep Reduction for Polyphony)
    0.80f, // Tri
    0.55f, // Master (Headroom for Delay)
    2.0f   // Release
};

// Speaker: Loud, Punchy, Short Tail
SoundProfile spkProfile = {
    0.19f, // Saw
    0.30f, // Square
    0.20f, // Sine (Deep Reduction)
    0.52f, // Tri
    0.65f, // Master
    2.0f   // Release
};

SoundProfile *currentProfile = &btProfile; // Default to BT settings mostly

Waveform currentWaveform = WAVE_SAW;

// --- Active Parameters ---
SynthParameters activeParams = {
    3500.0f,       // Filter Cutoff (Hz)
    0.70f,         // Resonance (0-1) - corresponds to q=0.3
    0.50f,         // WaveFold / PWM
    0.50f,         // Delay Feedback
    0.05f,         // Attack
    2.0f,          // Release
    0.35f,         // LFO Rate
    0.30f,         // LFO Depth (Default)
    0.20f,         // Drive Amount
    5.0f,          // Tremolo Rate
    LFO_SINE,      // LFO Type
    TARGET_FILTER, // LFO Target (Default)
    TARGET_FOLD    // LDR Target (Default)
};

// Waveform Presets (Active Params persisted per wave)
SynthParameters wavePresets[4];

// Forward declaration
void updateDerivedParameters();

void selectWaveform(int w) {
  if (w < 0 || w > 3)
    return;

  // Save Current
  wavePresets[currentWaveform] = activeParams;

  // Load New
  currentWaveform = (Waveform)w;
  activeParams = wavePresets[currentWaveform];

  updateDerivedParameters();
}

// --- Audio Buffer ---
#define AUDIO_BUF_SIZE 2048
volatile uint8_t audioBuffer[AUDIO_BUF_SIZE];
volatile int bufReadHead = 0;
volatile int bufWriteHead = 0;

volatile float globalPulseWidth = 0.5f;

// Filter State (Chamberlin SVF) - MOVED BELOW
// Removing duplicates caused by previous edit error
// Filter State (Chamberlin SVF)
// Filter State (Chamberlin SVF)
volatile float svf_low = 0;
volatile float svf_band = 0;
volatile float svf_f = 0.5f;  // Cutoff coefficient
volatile float svf_q = 0.22f; // Resonance

// Tape Wobble State
volatile float wowPhase = 0.0f;
volatile float flutterPhase = 0.0f;
const float wowInc = 2.0f * PI * 2.0f / SAMPLE_RATE; // 2.0Hz (Faster Wow)
const float flutterInc =
    2.0f * PI * 3.0f / SAMPLE_RATE; // 3.0Hz (Slower Flutter)

// Sound Design Tools State
bool fxDrive = false;
bool fxTrem = false;
bool fxLFO = false;

// Tremolo State
float tremPhase = 0.0f;
volatile float tremInc = 2.0f * PI * 5.0f / SAMPLE_RATE; // Updated by Params

// Global LFO State
float globalLfoPhase = 0.0f;
volatile float globalLfoVal = 0.0f;
volatile float globalLfoInc =
    2.0f * PI * 0.35f / SAMPLE_RATE; // Updated by Params

// Master Volume (User Controlled)
float masterVolume = 0.8f;

// Delay State
#define MAX_DELAY_MS 1200
#define DELAY_DOWNSAMPLE 4
#define MAX_DELAY_LEN                                                          \
  (int)(SAMPLE_RATE * MAX_DELAY_MS / 1000 / DELAY_DOWNSAMPLE)
// Using int16_t for buffer to save RAM (26KB now, super safe) + Super Vintage
// Grit
int16_t *delayBuffer = 0;
int delayHead = 0;
int delayTick = 0; // For downsampling
int delayMode = 0; // 0=Off, 1=300ms, 2=600ms, 3=900ms, 4=1200ms
volatile float wobbleDepth = 0.0025f; // Default 0.25%

// UI State
int lastTouchedString = -1;

// Helper for mapFloat
float mapFloat(float x, float in_min, float in_max, float out_min,
               float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Update Derived Parameters (Call after changing activeParams)
void updateDerivedParameters() {
  // Update Filter
  // svf_f = 2 * sin(PI * Fc / Fs)
  svf_f = 2.0f * sin(PI * activeParams.filterCutoff / (float)SAMPLE_RATE);

  // Update Resonance (q)
  // q = 1.0 - res. High Res = Low q.
  // Clamp Res to 0.95 to avoid explosion
  if (activeParams.filterRes > 0.95f)
    activeParams.filterRes = 0.95f;
  svf_q = 1.0f - activeParams.filterRes;

  // Update Rates
  globalLfoInc = 2.0f * PI * activeParams.lfoRate / (float)SAMPLE_RATE;
  tremInc = 2.0f * PI * activeParams.tremRate / (float)SAMPLE_RATE;

  // Update Voice Envelopes (Global update for simplicity)
  for (int i = 0; i < MAX_VOICES; i++) {
    voices[i].attackTime = activeParams.attackTime;
    voices[i].releaseTime = activeParams.releaseTime;
    // Re-calc rates
    voices[i].attackRate = 1.0f / (activeParams.attackTime * SAMPLE_RATE);
    voices[i].releaseRate = 1.0f / (activeParams.releaseTime * SAMPLE_RATE);
  }
}

// --- AUDIO GENERATION LOGIC (Shared) ---
// Returns a single float sample (-1.0 to 1.0) mixed from all voices + Filtered
float generateMixedSample(float pitchMod, float pwMod, float filterMod,
                          float resMod) {
  float mixedSample = 0.0f;
  int activeCount = 0;

  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active) {
      mixedSample += voices[i].getSample(pitchMod, pwMod);
      activeCount++;
    }
  }

  if (activeCount > 0) {
    // Dynamic Gain Scaling for Polyphony > 4
    float polyScale = 1.0f;
    if (activeCount > 4) {
      polyScale = 4.0f / (float)activeCount;
    }
    mixedSample *= (currentProfile->masterGain * polyScale);
  }

  // Anti-Denormal noise
  mixedSample += 1.0e-18f;

  // Apply Filter Modulation
  float f = svf_f * filterMod;
  if (f > 0.9f)
    f = 0.9f; // Stability limit
  if (f < 0.01f)
    f = 0.01f;

  svf_low += f * svf_band;

  // Waveform-dependent Resonance Tuning (Still useful, can combine with
  // activeParams?) User asked for "Resonance 1-90%". Let's use
  // activeParams.filterRes as base, modulated by resMod.

  float baseRes = activeParams.filterRes * resMod;
  if (baseRes > 0.95f)
    baseRes = 0.95f;
  if (baseRes < 0.01f)
    baseRes = 0.01f;

  // Convert Res to Q (damping). Low Damping = High Res.
  float q = 1.0f - baseRes;

  // Override for Saw if needed?
  // The user liked the "tuned" saw.
  // "Saw wave needs more damping (higher q)".
  // If user sets high resonance, Saw will squeal again.
  // Maybe apply a scaling factor for Saw?
  if (currentWaveform == WAVE_SAW) {
    q = q * 2.0f; // Double damping for Saw?
    if (q > 1.0f)
      q = 1.0f;
  }

  float high = mixedSample - svf_low - (q * svf_band);
  svf_band += f * high;

  // Clip (Hard Clip Filter States)
  if (svf_low > 2.0f)
    svf_low = 2.0f;
  else if (svf_low < -2.0f)
    svf_low = -2.0f;
  if (svf_band > 2.0f)
    svf_band = 2.0f;
  else if (svf_band < -2.0f)
    svf_band = -2.0f;

  return svf_low;
}

// --- BLUETOOTH CALLBACK (Always Compile) ---
// The A2DP library calls this to get data.
// Signature match: int32_t (*)(Frame *data, int32_t len) where len is frame
// count
int32_t bt_data_stream_callback(Frame *data, int32_t len) {
  if (delayBuffer == NULL)
    return len; // Safety Check

  for (int i = 0; i < len; i++) {
    // --- LFO Generation ---
    globalLfoPhase += globalLfoInc;
    if (globalLfoPhase >= 2.0f * PI)
      globalLfoPhase -= 2.0f * PI;

    float lfoVal = 0.0f;
    if (activeParams.lfoType == LFO_SINE) {
      lfoVal = sin(globalLfoPhase);
    } else if (activeParams.lfoType == LFO_SQUARE) {
      lfoVal = (globalLfoPhase < PI) ? 1.0f : -1.0f;
    } else if (activeParams.lfoType == LFO_RAMP) {
      lfoVal = (globalLfoPhase / PI) - 1.0f;
    } else if (activeParams.lfoType == LFO_NOISE) {
      lfoVal = (float)random(-100, 100) / 100.0f;
    }
    globalLfoVal = lfoVal;

    // --- Tape Wobble ---
    wowPhase += wowInc;
    if (wowPhase >= 6.283185307f)
      wowPhase -= 6.283185307f;
    flutterPhase += flutterInc;
    if (flutterPhase >= 6.283185307f)
      flutterPhase -= 6.283185307f;
    float wobble = (sin(wowPhase) + sin(flutterPhase)) * wobbleDepth;

    // --- Modulators ---
    float pitchMod = 1.0f + wobble;
    float pwMod = 0.0f;
    float filterMod = 1.0f;
    float resMod = 1.0f;

    if (fxLFO) {
      // Using same generic depth as Wired
      if (activeParams.lfoTarget == TARGET_PITCH) {
        pitchMod += lfoVal * 0.05f;
      } else if (activeParams.lfoTarget == TARGET_FOLD) {
        pwMod += lfoVal * 0.2f;
      } else if (activeParams.lfoTarget == TARGET_FILTER) {
        filterMod += lfoVal * 0.4f;
      } else if (activeParams.lfoTarget == TARGET_RES) {
        resMod += lfoVal * 0.3f;
      }
    }

    // Wave/Fold Parameter Mapping
    float totalPW = activeParams.waveFold;
    float derivedPwMod = (totalPW - 0.5f) + pwMod;

    float sample =
        generateMixedSample(pitchMod, derivedPwMod, filterMod, resMod);

    // FX: Drive
    if (fxDrive) {
      float drive = 1.0f + activeParams.driveAmount * 3.0f;
      sample *= drive;
      if (sample > 1.2f)
        sample = 1.2f;
      if (sample < -1.2f)
        sample = -1.2f;
      sample = sample - (sample * sample * sample) * 0.333f;
    }

    // Delay Processing
    delayTick++;
    if (delayTick >= DELAY_DOWNSAMPLE)
      delayTick = 0;

    float delayed = 0.0f;
    if (delayMode > 0 && delayBuffer != 0) {
      int delayMs = delayMode * 300;
      int delaySamples = (SAMPLE_RATE / DELAY_DOWNSAMPLE * delayMs) / 1000;
      int readPos = (delayHead - delaySamples + MAX_DELAY_LEN) % MAX_DELAY_LEN;
      delayed = (float)delayBuffer[readPos] * 3.3333e-5f;
    }

    float dry = sample;
    sample = dry + delayed * 0.5f;

    // FX: Tremolo
    if (fxTrem) {
      tremPhase += tremInc;
      if (tremPhase >= 6.283185307f)
        tremPhase -= 6.283185307f;
      float mod = 1.0f + 0.5f * sin(tremPhase);
      sample *= mod * 0.7f;
    }

    // Delay Write
    if (delayTick == 0 && delayBuffer != 0) {
      if (delayMode > 0) {
        float fbAmt = activeParams.delayFeedback;
        float fb = delayed * fbAmt + dry * 0.7f;
        if (fb > 1.0f)
          fb = 1.0f;
        if (fb < -1.0f)
          fb = -1.0f;
        delayBuffer[delayHead] = (int16_t)(fb * 30000.0f);
      } else {
        delayBuffer[delayHead] = 0;
      }
      delayHead = (delayHead + 1) % MAX_DELAY_LEN;
    }

    // Master Volume
    sample *= masterVolume;

    // Clip hard
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;

    // Scale to int16
    int16_t pcm = (int16_t)(sample * 30000.0f);

    // Stereo Frame
    data[i].channel1 = pcm; // Left
    data[i].channel2 = pcm; // Right
  }
  return len;
}

// --- WIRED/DAC CALLBACKS (Always Compile) ---

// --- ISR: Read Buffer ---
void ARDUINO_ISR_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);

  // Only play if buffer has data
  if (bufReadHead != bufWriteHead) {
    uint8_t sample = audioBuffer[bufReadHead];
    bufReadHead = (bufReadHead + 1) % AUDIO_BUF_SIZE;

    // Direct Register Write (Fastest)
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, sample,
                      RTC_IO_PDAC2_DAC_S);
  } else {
    // Underrun (Silence)? Or repeat last?
    // Doing nothing maintains last voltage.
  }

  // Underrun: Do nothing
  portEXIT_CRITICAL_ISR(&timerMux);
}

// --- Audio Generation Task (Called in loop) ---
// --- Audio Generation Task (Called in loop) ---
void fillAudioBuffer() {
  int samplesToFill = 256;

  while (samplesToFill > 0) {
    int nextWrite = (bufWriteHead + 1) % AUDIO_BUF_SIZE;
    if (nextWrite == bufReadHead)
      return; // Buffer Full

    // --- LFO Generation ---
    globalLfoPhase += globalLfoInc;
    if (globalLfoPhase >= 2.0f * PI)
      globalLfoPhase -= 2.0f * PI;

    float lfoVal = 0.0f;
    if (activeParams.lfoType == LFO_SINE) {
      lfoVal = sin(globalLfoPhase);
    } else if (activeParams.lfoType == LFO_SQUARE) {
      lfoVal = (globalLfoPhase < PI) ? 1.0f : -1.0f;
    } else if (activeParams.lfoType == LFO_RAMP) {
      // 0..2PI -> -1..1
      lfoVal = (globalLfoPhase / PI) - 1.0f;
    } else if (activeParams.lfoType == LFO_NOISE) {
      lfoVal = (float)random(-100, 100) / 100.0f;
    }
    globalLfoVal = lfoVal; // For UI

    // --- Tape Wobble ---
    wowPhase += wowInc;
    if (wowPhase >= 6.283185307f)
      wowPhase -= 6.283185307f;
    flutterPhase += flutterInc;
    if (flutterPhase >= 6.283185307f)
      flutterPhase -= 6.283185307f;
    float wobble = (sin(wowPhase) + sin(flutterPhase)) * wobbleDepth;

    // --- Modulators ---
    float pitchMod = 1.0f + wobble; // Default wobble always on pitch?
    float pwMod = 0.0f;
    float filterMod = 1.0f;
    float resMod = 1.0f;

    // --- LFO Routing ---
    // User requested LFO Target options.
    // FX LFO Toggle enables this routing.
    if (fxLFO) {
      // Use User Depth
      float depth = activeParams.lfoDepth;

      if (activeParams.lfoTarget == TARGET_PITCH) {
        pitchMod +=
            lfoVal * depth * 0.1f; // Scaled for Pitch (too wild otherwise)
      } else if (activeParams.lfoTarget == TARGET_FOLD) {
        pwMod += lfoVal * depth * 0.4f;
      } else if (activeParams.lfoTarget == TARGET_FILTER) {
        filterMod += lfoVal * depth * 0.8f;
      } else if (activeParams.lfoTarget == TARGET_RES) {
        resMod += lfoVal * depth * 0.5f;
      }
      // TARGET_ATTACK / TARGET_RELEASE handled in slow loop
    }

    // Waveform specific defaults if NO target selected?
    // Old behavior was: Sine->Pitch, Saw->Fold, Tri->Filter.
    // If user selects TARGET_NONE, maybe keep old behavior?
    // Or simpler: New system completely replaces old.

    // PW Mod needs base value from Params
    // activeParams.waveFold is 0.25-0.75.
    // pwMod is OFFSETS.
    // We pass `activeParams.waveFold + pwMod` to getSample?
    // No, getSample takes `pwMod` as the *modulation* or the *value*?
    // In Saw, `pwMod` is treated as fold depth.
    // In Square, `pwMod` is added to 0.5f.

    // Let's pass the TOTAL parameter to getSample.
    // Wait, getSample signature is `float pwMod`.
    // Inside Square: `float pw = 0.5f + pwMod`.
    // Inside Saw: `if (pwMod > 0.01f)`.
    // So `pwMod` implies "Amount of deviation from standard".

    // BUT user wants to SET "Wave Fold / PWM" amount.
    // So we should pass `(activeParams.waveFold - 0.5f) + LFO`?
    // Square: `0.5 + (val) = width`. If val is `activeParams.waveFold - 0.5`,
    // then width is `activeParams.waveFold`. Correct. Saw: `pwMod` is fold
    // gain? User wants "Wave Folder" 25-75%. If Saw, valid range is
    // 0..something. Let's pass `activeParams.waveFold` as the base if it's
    // meant to be the PARAMETER. I need to adjust `getSample` expectation or
    // adjust what I pass. Let's adjust what I pass.

    // Modulate PW/Fold
    // globalPulseWidth is now the EFFECITVE value (Param + LDR)
    float totalPW = globalPulseWidth;
    // Map to "Mod" expected by getSample
    // Square: 0.5 + mod = total -> mod = total - 0.5
    float derivedPwMod = (totalPW - 0.5f) + pwMod; // + LFO offset

    // For Saw: "if pwMod > 0.01". Fold depth.
    // using derivedPwMod: if totalPW is 0.5, mod is 0.
    // If totalPW is 0.75, mod is 0.25 (Heavy fold).
    // If totalPW is 0.25, mod is -0.25 (Negative fold? Saw ignores or handles?)
    // Saw check: `if (pwMod > 0.01f)`. So only > 0.5 PW settings engage Fold.

    float s = generateMixedSample(pitchMod, derivedPwMod, filterMod, resMod);

    // FX: Drive
    // Use activeParams.driveAmount
    if (fxDrive) {
      float drive = 1.0f + activeParams.driveAmount * 3.0f; // 1.0 to 2.8
      s *= drive;
      // Soft Clip
      if (s > 1.2f)
        s = 1.2f;
      if (s < -1.2f)
        s = -1.2f;
      s = s - (s * s * s) * 0.333f;
    }

    // Delay Processing
    // Use activeParams.delayFeedback
    delayTick++;
    if (delayTick >= DELAY_DOWNSAMPLE)
      delayTick = 0;

    float delayed = 0.0f;
    if (delayMode > 0 && delayBuffer != 0) {
      int delayMs = delayMode * 300;
      int delaySamples = (SAMPLE_RATE / DELAY_DOWNSAMPLE * delayMs) / 1000;
      int readPos = (delayHead - delaySamples + MAX_DELAY_LEN) % MAX_DELAY_LEN;
      delayed = (float)delayBuffer[readPos] * 3.3333e-5f;
    }

    float dry = s;
    s = dry + delayed * 0.5f; // Fixed Dry/Wet for now

    // FX: Tremolo
    if (fxTrem) {
      tremPhase += tremInc;
      if (tremPhase >= 6.283185307f)
        tremPhase -= 6.283185307f;
      float mod = 1.0f + 0.5f * sin(tremPhase);
      s *= mod * 0.7f;
    }

    // Write Back
    if (delayTick == 0 && delayBuffer != 0) {
      if (delayMode > 0) {
        // Feedback from Params
        float fbAmt = activeParams.delayFeedback;
        float fb = delayed * fbAmt + dry * 0.7f;
        if (fb > 1.0f)
          fb = 1.0f;
        if (fb < -1.0f)
          fb = -1.0f;
        delayBuffer[delayHead] = (int16_t)(fb * 30000.0f);
      } else {
        delayBuffer[delayHead] = 0;
      }
      delayHead = (delayHead + 1) % MAX_DELAY_LEN;
    }

    // Apply Master Volume
    s *= masterVolume;

    // Clip
    if (s > 1.0f)
      s = 1.0f;
    if (s < -1.0f)
      s = -1.0f;

    // Map float -1..1 to 0..255 (8-bit)
    int dacValue = (int)((s + 1.0f) * 127.5f);
    if (dacValue < 0)
      dacValue = 0;
    if (dacValue > 255)
      dacValue = 255;

    audioBuffer[bufWriteHead] = (uint8_t)dacValue;
    bufWriteHead = nextWrite;
    samplesToFill--;
  }
}

// --- UI Functions (Forward Declared implicitly by ordering) ---
// --- UI Functions (Forward Declared implicitly by ordering) ---
void drawWaveButton();      // Forward decl due to ordering
void updateButtonVisuals(); // Forward decl for handleButtonPress
// Actually, let's keep the user's order: Includes -> Globals -> ISR -> UI ->
// Setup -> Loop But we need fillAudioBuffer defined before Loop.

// --- UI Functions (Forward Declared implicitly by ordering) ---

const char *noteNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                           "F#", "G",  "G#", "A",  "A#", "B"}; // cycling
float stringEnergy[STRING_COUNT] = {0};       // 0.0 to 1.0 for visual fade
uint16_t lastStringColor[STRING_COUNT] = {0}; // Cache to unnecessary redraws

// Helper to blend colors
uint16_t alphaBlend(uint16_t c1, uint16_t c2, float alpha) {
  // Extract RGB565
  int r1 = (c1 >> 11) & 0x1F;
  int g1 = (c1 >> 5) & 0x3F;
  int b1 = c1 & 0x1F;
  int r2 = (c2 >> 11) & 0x1F;
  int g2 = (c2 >> 5) & 0x3F;
  int b2 = c2 & 0x1F;

  int r = r1 + (r2 - r1) * alpha;
  int g = g1 + (g2 - g1) * alpha;
  int b = b1 + (b2 - b1) * alpha;

  return (r << 11) | (g << 5) | b;
}

bool isBlackKey(int index) {
  int note = index % 12;
  return (note == 1 || note == 3 || note == 6 || note == 8 || note == 10);
}

void updateStringVisuals() {
  tft.setTextDatum(TC_DATUM);
  bool redrawBtn = false;

  for (int i = 0; i < STRING_COUNT; i++) {
    // Calculate dynamic width to fill screen perfectly
    int x = (i * SCREEN_WIDTH) / STRING_COUNT;
    int nextX = ((i + 1) * SCREEN_WIDTH) / STRING_COUNT;
    int w = nextX - x;

    // 1. Decay Energy
    if (stringEnergy[i] > 0.01f) {
      stringEnergy[i] *= 0.95f; // Decay speed
    } else {
      stringEnergy[i] = 0.0f;
    }

    // 2. Determine Color
    bool isBlack = isBlackKey(i);
    uint16_t baseColor = isBlack ? COLOR_STRING_BLACK : COLOR_STRING_WHITE;
    uint16_t activeColor = TFT_ORANGE; // Highlight color

    uint16_t currentColor =
        stringEnergy[i] > 0
            ? alphaBlend(baseColor, activeColor, stringEnergy[i])
            : baseColor;

    // 3. Draw ONLY if color changed significantly (to save SPI bandwidth)
    if (currentColor != lastStringColor[i]) {
      // Reserve Top Bar for Buttons (Always offset by 40px)
      // Reserve Bottom Gap for Slider (180-200) + Buttons (200-240)
      // So strings draw 40 -> 180.
      int drawY = 40;
      int drawH = 140; // 180 - 40

      tft.fillRect(x, drawY, w, drawH, currentColor);
      tft.drawRect(x, drawY, w, drawH, TFT_DARKGREY);

      // Only draw label for C notes (every 12th)
      if (i % 12 == 0) {
        uint16_t textColor = isBlack ? TFT_WHITE : TFT_BLACK;
        // If highly active, maybe make text white always?
        if (stringEnergy[i] > 0.5f)
          textColor = TFT_WHITE;

        // Ensure label is visible
        int labelY = drawY + 5;

        tft.setTextColor(textColor);
        tft.drawString("C", x + (w / 2), labelY);
      }
      lastStringColor[i] = currentColor;
    }
  }
}

void drawStrings() {
  // Force full redraw by setting lastColor to a value that definitely isn't a
  // string color 0xFFFF is White, which matched our white strings, causing
  // them to NOT draw.
  for (int i = 0; i < STRING_COUNT; i++)
    lastStringColor[i] = 0x1234; // random color
  updateStringVisuals();
}

// Chord Logic
// 6 Buttons x 4 Banks = 24 Chords
// Root is bit 0.
uint8_t buttonStates[8] = {
    0}; // Track bank 0-3 for each button (indices 1-6 used)

const char *chordLabels[6][5] = {
    {"Maj", "Maj7", "Inv 1", "Inv 2", "Inv 3"}, // Btn 1
    {"Min", "Min7", "Inv 1", "Inv 2", "Inv 3"}, // Btn 2
    {"Dim", "Dim7", "Inv 1", "Inv 2", "Inv 3"}, // Btn 3
    {"7th", "9th", "Inv 1", "Inv 2", "Inv 3"},  // Btn 4
    {"Aug", "Aug7", "Inv 1", "Inv 2", "Inv 3"}, // Btn 5
    {"Sus4", "Sus2", "Inv 1", "Inv 2", "Inv 3"} // Btn 6
};

// Bitmasks (0=C, 1=C#, etc.) - Normalized to 0-11 range
// Bitmasks (0=C, 1=C#, etc.) - Normalized to 0-11 range
// Bank 1: Original. Bank 2: Original. Bank 3: Inv1. Bank 4: Inv2 (Spicy). Bank
// 5: Inv3 (Spicy).
const uint16_t chordMasks[6][5] = {
    // Btn 1: Majors
    {(1 << 0) | (1 << 4) | (1 << 7),                        // Maj
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 11),            // Maj7
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 11),            // Maj7 (Inv1)
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 11) | (1 << 2), // Maj9 (Inv2 + 9th)
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 11) |
         (1 << 9)}, // Maj13 (Inv3 + 13th)

    // Btn 2: Minors
    {(1 << 0) | (1 << 3) | (1 << 7),                        // Min
     (1 << 0) | (1 << 3) | (1 << 7) | (1 << 10),            // Min7
     (1 << 0) | (1 << 3) | (1 << 7) | (1 << 10),            // Min7 (Inv1)
     (1 << 0) | (1 << 3) | (1 << 7) | (1 << 10) | (1 << 2), // Min9 (Inv2 + 9th)
     (1 << 0) | (1 << 3) | (1 << 7) | (1 << 10) |
         (1 << 5)}, // Min11 (Inv3 + 11th)

    // Btn 3: Diminished
    {(1 << 0) | (1 << 3) | (1 << 6),                         // Dim
     (1 << 0) | (1 << 3) | (1 << 6) | (1 << 9),              // Dim7
     (1 << 0) | (1 << 3) | (1 << 6) | (1 << 9),              // Dim7 (Inv1)
     (1 << 0) | (1 << 3) | (1 << 6) | (1 << 9) | (1 << 2),   // Dim7+9 (Inv2)
     (1 << 0) | (1 << 3) | (1 << 6) | (1 << 9) | (1 << 11)}, // DimMaj7 (Inv3)

    // Btn 4: Dominant
    {(1 << 0) | (1 << 4) | (1 << 7) | (1 << 10),            // 7th
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 10) | (1 << 2), // 9th
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 10) | (1 << 2), // 9th (Inv1)
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 10) | (1 << 2) |
         (1 << 9), // 13th (Inv2 + 9+13)
     (1 << 0) | (1 << 4) | (1 << 7) | (1 << 10) |
         (1 << 9)}, // 7add13 (Inv3 + 13th)

    // Btn 5: Augmented
    {(1 << 0) | (1 << 4) | (1 << 8),                        // Aug
     (1 << 0) | (1 << 4) | (1 << 8) | (1 << 10),            // Aug7
     (1 << 0) | (1 << 4) | (1 << 8) | (1 << 10),            // Aug7 (Inv1)
     (1 << 0) | (1 << 4) | (1 << 8) | (1 << 10) | (1 << 2), // Aug9 (Inv2 + 9th)
     (1 << 0) | (1 << 4) | (1 << 8) | (1 << 10) |
         (1 << 6)}, // Aug7#11 (Inv3 + #11)

    // Btn 6: Suspended
    {(1 << 0) | (1 << 5) | (1 << 7),            // Sus4
     (1 << 0) | (1 << 2) | (1 << 7),            // Sus2
     (1 << 0) | (1 << 2) | (1 << 7),            // Sus2 (Inv1)
     (1 << 0) | (1 << 2) | (1 << 7) | (1 << 9), // Sus2+6 (Inv2)
     (1 << 0) | (1 << 2) | (1 << 7) | (1 << 4)} // Sus2+3 (Inv3 - Hybrid?)
};

// Helper to find cutoff for inversion (0=None, 1=1st Note, 2=2nd Note...)
int getInversionCutoff(uint16_t mask, int inversion) {
  if (inversion <= 0)
    return 0;

  int foundNotes = 0;
  for (int i = 0; i < 12; i++) {
    if (mask & (1 << i)) {
      // Found a note
      if (foundNotes == inversion) {
        return i; // This is the new bass note
      }
      foundNotes++;
    }
  }
  return 0; // Fallback
}

// --- UI HELPERS ---
// Layout: 5 Buttons across top (320px / 5 = 64px each)
// [Delay] [Drive] [Trem] [LFO] [Wave]

void drawWaveButton() {
  int x = 259; // Slot 4 (256) + 3 padding
  int y = 5;
  int w = 58; // 64 - 6 padding
  int h = 30;

  tft.fillRect(x, y, w, h, TFT_DARKGREY);
  tft.drawRect(x, y, w, h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);

  // Draw Icon (Full Width: 50px, 2 Cycles of 25px each)
  int x0 = x + 5;
  int x1 = x0 + 12; // Peak 1
  int x2 = x0 + 25; // End Cycle 1
  int x3 = x0 + 37; // Peak 2
  int x4 = x0 + 50; // End Cycle 2

  int yTop = y + 5;
  int yBot = y + h - 5;
  int yMid = y + h / 2;

  if (currentWaveform == WAVE_SAW) {
    // Sawtooth Line: /|/|
    tft.drawLine(x0, yBot, x2, yTop, TFT_WHITE); // Ramp 1
    tft.drawLine(x2, yTop, x2, yBot, TFT_WHITE); // Drop 1
    tft.drawLine(x2, yBot, x4, yTop, TFT_WHITE); // Ramp 2
    tft.drawLine(x4, yTop, x4, yBot, TFT_WHITE); // Drop 2
  } else if (currentWaveform == WAVE_SQUARE) {
    // Square Line: |_|-|_| (2 Cycles)
    // C1: x0..x2. High x0..x1, Low x1..x2.
    // Actually typically High first.
    // Up, High, Down, Low.
    tft.drawLine(x0, yBot, x0, yTop, TFT_WHITE); // Up
    tft.drawLine(x0, yTop, x1, yTop, TFT_WHITE); // High
    tft.drawLine(x1, yTop, x1, yBot, TFT_WHITE); // Down
    tft.drawLine(x1, yBot, x2, yBot, TFT_WHITE); // Low
    // Cycle 2
    tft.drawLine(x2, yBot, x2, yTop, TFT_WHITE); // Up
    tft.drawLine(x2, yTop, x3, yTop, TFT_WHITE); // High
    tft.drawLine(x3, yTop, x3, yBot, TFT_WHITE); // Down
    tft.drawLine(x3, yBot, x4, yBot, TFT_WHITE); // Low
  } else if (currentWaveform == WAVE_SINE) {
    // Sine Wave Icon (1 Cycle - Simple)
    int prevY = yMid;
    for (int i = 0; i <= 50; i++) {
      // 1 cycle over 50px -> 2PI
      float angle = (i / 50.0f) * 6.28318f;
      int newY = yMid - (int)(sin(angle) * 10.0f);
      if (i > 0)
        tft.drawLine(x0 + i - 1, prevY, x0 + i, newY, TFT_WHITE);
      prevY = newY;
    }
  } else if (currentWaveform == WAVE_TRIANGLE) {
    // Triangle: /\/\ (2 Cycles)
    // Points: 0, 12, 25, 37, 50
    tft.drawLine(x0, yBot, x1, yTop, TFT_WHITE); // Up 1
    tft.drawLine(x1, yTop, x2, yBot, TFT_WHITE); // Down 1
    tft.drawLine(x2, yBot, x3, yTop, TFT_WHITE); // Up 2
    tft.drawLine(x3, yTop, x4, yBot, TFT_WHITE); // Down 2
  }
}

void drawDelayButton() {
  int x = 3; // Slot 0 (0) + 3 padding
  int y = 5;
  int w = 58;
  int h = 30;

  uint16_t color = TFT_DARKGREY;
  const char *label = "Delay";

  if (delayMode == 0) {
    color = TFT_LIGHTGREY; // Off state
    label = "Delay";
  } else if (delayMode == 1) {
    color = TFT_GREEN;
    label = "300";
  } else if (delayMode == 2) {
    color = TFT_CYAN;
    label = "600";
  } else if (delayMode == 3) {
    color = TFT_YELLOW;
    label = "900";
  } else if (delayMode == 4) {
    color = TFT_MAGENTA;
    label = "1200";
  }

  tft.fillRect(x, y, w, h, color);
  tft.drawRect(x, y, w, h, TFT_WHITE);

  tft.setTextColor(delayMode > 0 ? TFT_BLACK : TFT_WHITE);
  tft.setTextDatum(MC_DATUM);

  // Use slightly smaller font for "Delay" if needed
  if (delayMode == 0)
    tft.setTextSize(1);
  else
    tft.setTextSize(1);

  if (delayMode == 0)
    tft.setTextSize(1);
  else
    tft.setTextSize(1);

  tft.drawString(label, x + (w / 2), y + (h / 2));
}

void drawFXButtons() {
  int y = 5;
  int w = 58;
  int h = 30;

  // Drive (Btn 1: Slot 64) -> x=67
  int xDrive = 67;
  uint16_t cDrive = fxDrive ? TFT_RED : TFT_DARKGREY;
  tft.fillRect(xDrive, y, w, h, cDrive);
  tft.drawRect(xDrive, y, w, h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Drive", xDrive + w / 2, y + h / 2);

  // Trem (Btn 2: Slot 128) -> x=131
  int xTrem = 131;
  uint16_t cTrem = fxTrem ? TFT_ORANGE : TFT_DARKGREY;
  tft.fillRect(xTrem, y, w, h, cTrem);
  tft.drawRect(xTrem, y, w, h, TFT_WHITE);
  tft.drawString("Trem", xTrem + w / 2, y + h / 2);

  // LFO (Btn 3: Slot 192) -> x=195
  int xLFO = 195;
  uint16_t cLFO = fxLFO ? TFT_MAGENTA : TFT_DARKGREY;
  tft.fillRect(xLFO, y, w, h, cLFO);
  tft.drawRect(xLFO, y, w, h, TFT_WHITE);
  tft.drawString("LFO", xLFO + w / 2, y + h / 2);
}

void drawVolumeSlider() {
  int y = 180;
  int h = 20;
  int w = SCREEN_WIDTH;

  // Clear Area
  tft.fillRect(0, y, w, h, TFT_BLACK); // Background

  // Draw Dotted Line
  int midY = y + h / 2;
  for (int i = 10; i < w - 10; i += 4) {
    tft.drawPixel(i, midY, TFT_DARKGREY);
  }

  // Draw Ball
  // Map Volume 0..1 to 15..w-15
  int ballX = mapFloat(masterVolume, 0.0f, 1.0f, 15, w - 15);
  tft.fillCircle(ballX, midY, 6, TFT_CYAN);
  tft.drawCircle(ballX, midY, 6, TFT_WHITE);
}

void updateButtonVisuals() {
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  // 8 Buttons at BOTTOM
  int btnCount = 8;
  int w = SCREEN_WIDTH / btnCount;
  int h = 40;
  int y = SCREEN_HEIGHT - h; // Bottom

  // Bank Colors: Green, Cyan, Yellow, Magenta, Orange, Grey (Off)
  uint16_t bankColors[] = {TFT_GREEN,   TFT_CYAN,   TFT_YELLOW,
                           TFT_MAGENTA, TFT_ORANGE, TFT_LIGHTGREY};

  for (int i = 0; i < btnCount; i++) {
    int x = i * w;
    uint16_t color = COLOR_BTN_INACTIVE;
    const char *label = "";
    int textColor = TFT_WHITE;

    if (i == 0) { // Octave Down
      label = "<";
      if (octaveShift < 0)
        color = alphaBlend(COLOR_BTN_INACTIVE, COLOR_BTN_ACTIVE,
                           abs(octaveShift) / 3.0f);
    } else if (i == 7) { // Octave Up
      label = ">";
      if (octaveShift > 0)
        color = alphaBlend(COLOR_BTN_INACTIVE, COLOR_BTN_ACTIVE,
                           abs(octaveShift) / 3.0f);
    } else {
      // Chord Buttons (1-6)
      if (activeButtonIndex == -1) { // Chromatic mode
        label = "Off";
        color = TFT_LIGHTGREY;
      } else if (activeButtonIndex == i) {
        label = chordLabels[i - 1][buttonStates[i]];
        color = bankColors[buttonStates[i]];
        textColor = TFT_BLACK;
      } else {
        label = chordLabels[i - 1][buttonStates[i]];
        color = COLOR_BTN_INACTIVE;
      }
    }

    tft.fillRect(x, y, w, h, color);
    tft.drawRect(x, y, w, h, TFT_WHITE);
    tft.setTextColor(textColor);
    tft.drawString(label, x + (w / 2), y + (h / 2));
  }
}

void drawInterface() {
  drawStrings();
  drawWaveButton();
  drawDelayButton();
  drawFXButtons();
  drawVolumeSlider();
  updateButtonVisuals();
}

void handleButtonPress(int index) {
  // Index 0-7 (Octave/Chords)
  if (index == 0) { // Octave Down
    octaveShift--;
    if (octaveShift < -3)
      octaveShift = -3;
  } else if (index == 7) { // Octave Up
    octaveShift++;
    if (octaveShift > 3)
      octaveShift = 3;
  } else {
    // Chord Button (1-6)
    // No need to map, logic uses 1-6 directly
    if (activeButtonIndex == index) {
      // Cycle State
      buttonStates[index] = (buttonStates[index] + 1) % 5;
    } else {
      if (activeButtonIndex != -1) {
        activeButtonIndex = -1;
        currentChordMask = 0xFFFF;
        updateButtonVisuals();
        delay(150); // Debounce visual
        return;
      } else {
        activeButtonIndex = index;
      }
    }

    if (activeButtonIndex == -1) {
      currentChordMask = 0xFFFF;
    } else {
      int state = buttonStates[index];
      currentChordMask = chordMasks[index - 1][state];
    }
  }

  // Log Button Press
  if (index >= 1 && index <= 6) {
    int s = buttonStates[index];
    const char *l = chordLabels[index - 1][s];
    Serial.printf("Bottom Btn %d: %s (Mask: 0x%X)\n", index, l,
                  currentChordMask);
  } else {
    Serial.printf("Octave Btn: %d (Shift: %d)\n", index, octaveShift);
  }

  updateButtonVisuals(); // Refresh all
  delay(150);            // Debounce
}

// --- SETUP FUNCTIONS ---
void setupSpeaker() {
  Serial.println("Initializing Speaker (DAC)...");

  // Set Profile
  currentProfile = &spkProfile;
  Serial.println("Sound Profile: SPEAKER (High Output)");

  // Clear buffer
  for (int i = 0; i < AUDIO_BUF_SIZE; i++)
    audioBuffer[i] = 128;

  // Init DAC hardware (Enable output)
  dacWrite(AUDIO_PIN, 128); // Initialize at mid-rail

  // Init Audio Timer
  // Use Prescaler 8 (10MHz Clock) for better divisor resolution at 44.1kHz
  // 1MHz (Prescaler 80) -> 1000000/44100 = 22 ticks -> 45.4kHz (+3% pitch
  // error) 10MHz (Prescaler 8) -> 10000000/44100 = 227 ticks -> 44.05kHz (-0.1%
  // pitch error)
  timer = timerBegin(0, 8, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 10000000 / SAMPLE_RATE, true);
  timerAlarmEnable(timer);
}

// BT Connection State Callback
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  Serial.println(a2dp_source.to_str(state));
}

// --- BT Selection Globals ---
struct BTDevice {
  char name[64];
  int rssi;
};
BTDevice foundDevices[6]; // 5 Slots + 1 buffer
int foundDeviceCount = 0;
char targetSSID[64] = ""; // Target to connect to

// BT Scanning Callback
bool ssid_callback(const char *ssid, esp_bd_addr_t bd_addr, int rssi) {
  if (ssid == NULL || strlen(ssid) == 0)
    return false;

  // 1. Connection Mode: User has selected a device
  if (strlen(targetSSID) > 0) {
    if (strcmp(ssid, targetSSID) == 0) {
      Serial.printf("Target Found! Connecting to %s\n", ssid);
      return true; // Stop scan, Connect
    }
    return false;
  }

  // 2. Scanning Mode: Populate List
  // Check duplicates
  for (int i = 0; i < foundDeviceCount; i++) {
    if (strcmp(foundDevices[i].name, ssid) == 0) {
      // Update RSSI if better
      if (rssi > foundDevices[i].rssi) {
        foundDevices[i].rssi = rssi;
      }
      return false; // Already listed
    }
  }

  // Add new device if slots available
  if (foundDeviceCount < 5) {
    strncpy(foundDevices[foundDeviceCount].name, ssid, 63);
    foundDevices[foundDeviceCount].rssi = rssi;
    foundDeviceCount++;
    Serial.printf("Discovered: %s (%d)\n", ssid, rssi);
  }

  return false; // Keep scanning
}

void setupBluetooth(bool scanning = false) {
  Serial.println("Initializing Bluetooth...");

  // Set Profile (Only if not scanning? Actually scanning uses same profile)
  currentProfile = &btProfile;
  Serial.println("Sound Profile: BLUETOOTH (High Fidelity)");

  // Power Cycle BLE to clear stuck state
  a2dp_source.set_reset_ble(true);

  // Setup Callbacks & config
  a2dp_source.set_on_connection_state_changed(connection_state_changed);
  a2dp_source.set_ssid_callback(ssid_callback);
  a2dp_source.set_data_callback_in_frames(
      bt_data_stream_callback); // Register Audio Source

  // CRITICAL FIX: Disable auto-reconnect during scanning
  // Otherwise it tries to connect to last device and ignores new scans
  if (scanning) {
    a2dp_source.set_auto_reconnect(false);
  } else {
    a2dp_source.set_auto_reconnect(true);
  }

  // Start connection (Empty start triggers scan if ssid_callback is set)
  a2dp_source.start();
  a2dp_source.set_volume(100);

  delay(1000); // Give radio time to initialize

  // CRITICAL: Re-Init Touch Hardware
  // Bluetooth start likely grabbed GPIO 25 (DAC1) which is shared with Touch
  // CLK. We must steal it back!
  Serial.println("Re-claiming SPI/Touch pins...");
  SPI.end();
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  // Restore High Speed SPI for TFT Performance
  SPI.setFrequency(27000000);
  ts.begin();
  ts.setRotation(1);

  Serial.println("Bluetooth Scanning Started...");
}

// --- APP MODES ---
enum AppMode { MODE_PLAY, MODE_EDIT };
AppMode currentMode = MODE_PLAY;
bool editorInputBlocked = false; // Prevent bounce on entry
uint32_t wavePressStart = 0;     // Global for Editor Long Press

// --- EDITOR UI HELPERS ---

// Helper to draw a slider control
void drawSliderControl(int x, int y, int w, int h, const char *label, float val,
                       float min, float max, bool isEnum = false) {
  // Clear background first to prevent artifacts
  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_DARKGREY);

  // Label
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + 10);

  // Visual
  if (isEnum) {
    // Value Text
    String s = String((int)val);
    // Custom Enum Lookups
    if (String(label) == "LFO Type") {
      tft.setTextColor(TFT_WHITE);
      int cx = x + w / 2;
      int cy = y + h / 2 + 5;
      int sz = 12; // Icon size

      if (val == 0) { // SINE
        // Draw simple sine approx
        for (int i = -sz; i < sz; i++) {
          float ang = (float)i / (float)sz * PI;
          float sy = sin(ang) * (float)sz / 2;
          tft.drawPixel(cx + i, cy - (int)sy, TFT_WHITE);
          tft.drawPixel(cx + i, cy - (int)sy - 1, TFT_WHITE); // Thicken
        }
      } else if (val == 1) {                                   // SQUARE
        tft.drawRect(cx - sz, cy - sz / 2, sz, sz, TFT_WHITE); // Top half? No
        // |_|~
        tft.drawLine(cx - sz, cy - sz / 2, cx, cy - sz / 2, TFT_WHITE); // High
        tft.drawLine(cx, cy - sz / 2, cx, cy + sz / 2, TFT_WHITE);      // Drop
        tft.drawLine(cx, cy + sz / 2, cx + sz, cy + sz / 2, TFT_WHITE); // Low
      } else if (val == 2) {                                            // RAMP
        tft.drawLine(cx - sz, cy + sz / 2, cx + sz, cy - sz / 2, TFT_WHITE);
        tft.drawLine(cx + sz, cy - sz / 2, cx + sz, cy + sz / 2, TFT_WHITE);
      } else if (val == 3) { // NOISE
        for (int i = -sz; i < sz; i += 2) {
          int rO = random(-sz / 2, sz / 2);
          tft.drawPixel(cx + i, cy + rO, TFT_WHITE);
        }
      }
    } else if (String(label) == "Target" || String(label) == "LDR Tgt" ||
               String(label) == "LFO Tgt") {
      if (val == 0)
        s = "NONE";
      if (val == 1)
        s = "FOLD";
      if (val == 2)
        s = "FILT";
      if (val == 3)
        s = "RES";
      if (val == 4)
        s = "PITC";
      if (val == 5)
        s = "ATK";
      if (val == 6)
        s = "REL";
      tft.setTextSize(2);
      tft.drawString(s, x + w / 2, y + h / 2 + 5);
      tft.setTextSize(1);
    } else { // Fallback for other enums
      tft.setTextSize(2);
      tft.drawString(s, x + w / 2, y + h / 2 + 5);
      tft.setTextSize(1);
    }
  } else {
    // Slider Bar
    int barW = w - 10;
    int barX = x + 5;
    int barY = y + 25;
    int barH = h - 35;

    // Track
    tft.drawRect(barX, barY, barW, barH, TFT_DARKGREY);

    // Knob / Fill
    float norm = (val - min) / (max - min);
    int fillW = (int)(norm * barW);
    tft.fillRect(barX, barY, fillW, barH, TFT_CYAN);

    // Value Overlay
    tft.setTextColor(TFT_BLACK, TFT_CYAN); // Invert
    // Adjust precision
    String s = String(val);
    if (max > 100)
      s = String((int)val); // Int for Hz
    else if (max < 5)
      s = String(val, 2); // Float for small

    // Center text in bar? Or below?
    // tft.drawString(s, x + w/2, y + h - 10);
  }
}

void drawPianoButton(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, TFT_WHITE);
  tft.drawRect(x, y, w, h, TFT_BLACK);
  // Keyboard Keys
  int kw = w / 5;
  for (int i = 0; i < 5; i++) {
    tft.drawRect(x + i * kw, y, kw, h, TFT_BLACK); // White keys
    if (i != 2) {
      tft.fillRect(x + i * kw + kw / 2 + 2, y, kw / 2, h / 2,
                   TFT_BLACK); // Black keys
    }
  }

  // Waveform Indicator (Top Left Corner)
  int wx = x + 4;
  int wy = y + 4;
  int wz = 10;

  tft.fillRect(wx, wy, 20, 15, TFT_BLACK); // BG

  if (currentWaveform == WAVE_SAW) {
    tft.drawLine(wx + 2, wy + 12, wx + 18, wy + 2, TFT_WHITE);
    tft.drawLine(wx + 18, wy + 2, wx + 18, wy + 12, TFT_WHITE);
  } else if (currentWaveform == WAVE_SQUARE) {
    tft.drawLine(wx + 2, wy + 2, wx + 10, wy + 2, TFT_WHITE);
    tft.drawLine(wx + 10, wy + 2, wx + 10, wy + 12, TFT_WHITE);
    tft.drawLine(wx + 10, wy + 12, wx + 18, wy + 12, TFT_WHITE);
  } else if (currentWaveform == WAVE_SINE) {
    for (int i = 0; i < 16; i++) {
      float sy = sin((float)i / 16.0 * TWO_PI) * 5.0;
      tft.drawPixel(wx + 2 + i, wy + 7 - (int)sy, TFT_WHITE);
    }
  } else if (currentWaveform == WAVE_TRIANGLE) {
    tft.drawLine(wx + 2, wy + 12, wx + 10, wy + 2, TFT_WHITE);
    tft.drawLine(wx + 10, wy + 2, wx + 18, wy + 12, TFT_WHITE);
  }
}

// --- EDITOR UI: Refined Layout ---
// --- EDITOR UI: Refined Layout ---
void drawEditor() {
  // Top Row (0): 4 Cols
  // Col 0: PIANO (Exit)
  // Col 1: LFO Tgt (was Trigger)
  // Col 2: LFO Shape
  // Col 3: LDR Tgt

  int topW = SCREEN_WIDTH / 4;
  int topH = SCREEN_HEIGHT / 4;
  int topY = 0;

  drawPianoButton(0 * topW, topY, topW, topH);
  drawSliderControl(1 * topW, topY, topW, topH, "LFO Tgt",
                    (float)activeParams.lfoTarget, 0, 6, true);
  drawSliderControl(2 * topW, topY, topW, topH, "LFO Type",
                    (float)activeParams.lfoType, 0, 3, true);
  drawSliderControl(3 * topW, topY, topW, topH, "LDR Tgt",
                    (float)activeParams.ldrTarget, 0, 6, true);

  // Bottom Rows (1,2,3): 3 Cols
  int botW = SCREEN_WIDTH / 3;
  int botH = SCREEN_HEIGHT / 4;

  // Row 1 (y = topH) -> LFO Hz, LFO Depth, Drive
  int r1 = topH;
  drawSliderControl(0 * botW, r1, botW, botH, "LFO Hz", activeParams.lfoRate,
                    0.125, 10.0);
  drawSliderControl(1 * botW, r1, botW, botH, "LFO Depth",
                    activeParams.lfoDepth, 0.0, 1.0);
  drawSliderControl(2 * botW, r1, botW, botH, "Drive", activeParams.driveAmount,
                    0.05, 0.60);

  // Row 2 (y = topH * 2) -> Cutoff, Res, Dly FB
  int r2 = topH * 2;
  drawSliderControl(0 * botW, r2, botW, botH, "Cutoff",
                    activeParams.filterCutoff, 100, 4000);
  drawSliderControl(1 * botW, r2, botW, botH, "Res", activeParams.filterRes,
                    0.01, 0.90);
  drawSliderControl(2 * botW, r2, botW, botH, "Dly FB",
                    activeParams.delayFeedback, 0.05, 0.70);

  // Row 3 (y = topH * 3) -> Attack, Release, Trem Hz
  int r3 = topH * 3;
  drawSliderControl(0 * botW, r3, botW, botH, "Attack", activeParams.attackTime,
                    0.001, 0.150);
  drawSliderControl(1 * botW, r3, botW, botH, "Release",
                    activeParams.releaseTime, 0.100, 3.000);
  drawSliderControl(2 * botW, r3, botW, botH, "Trem Hz", activeParams.tremRate,
                    0.25, 5.0);
}

// --- EDITOR INPUT HANDLER ---
uint32_t editorPianoPressStart = 0;
uint32_t delayPressStart = 0;
bool editorPianoHandled = false;
uint32_t inputBlockTimer = 0; // Blocks Play Mode Inputs
uint32_t editorPianoBlockTimer =
    0; // Blocks new Editor Piano presses (Debounce)

void handleEditorTouch(int tx, int ty) {
  int topH = SCREEN_HEIGHT / 4;
  int topW = SCREEN_WIDTH / 4;
  int botH = SCREEN_HEIGHT / 4;
  int botW = SCREEN_WIDTH / 3;

  // Variables for Slider Logic
  float *sliderTarget = 0;
  float minV = 0, maxV = 1;
  const char *label = "";
  int rx = 0, ry = 0, rw = 0, rh = 0;

  // --- TOP ROW (Buttons/Enums) ---
  if (ty < topH) {
    int idx = tx / topW;
    // Bounds check
    if (idx < 0)
      idx = 0;
    if (idx > 3)
      idx = 3;

    // Calculate generic Rect for redraw
    rx = idx * topW;
    ry = 0;
    rw = topW;
    rh = topH;

    // DEBOUNCE logic for buttons
    static uint32_t lastTopPress = 0;
    if (idx != 0 && millis() - lastTopPress < 250)
      return; // Debounce Enums

    switch (idx) {
    case 0: // PIANO / EXIT / SWITCH
    {
      if (millis() < editorPianoBlockTimer)
        return; // Ignore bounces after action

      if (editorPianoPressStart == 0) {
        editorPianoPressStart = millis();
        editorPianoHandled = false;
      }

      // Visual Feedback
      tft.drawRect(rx, ry, rw, rh, TFT_YELLOW);

      // Hold > 700ms -> Switch Wave
      if (!editorPianoHandled && millis() - editorPianoPressStart > 700) {
        int next = (currentWaveform + 1) % 4;
        selectWaveform(next);

        // Redraw Editor
        tft.fillScreen(COLOR_BG);
        drawEditor();

        // Mark Handled and Block new inputs
        editorPianoHandled = true;
        editorPianoBlockTimer = millis() + 500; // Block bounces for 500ms
      }
    }
      return; // Return so we don't fall through
    case 1:   // LFO Tgt
    {
      int v = (int)activeParams.lfoTarget;
      v++;
      if (v > 6)
        v = 0;
      activeParams.lfoTarget = (LfoTarget)v;
      drawSliderControl(rx, ry, rw, rh, "LFO Tgt", (float)v, 0, 6, true);
      lastTopPress = millis();
    } break;
    case 2: // LFO Type
    {
      int v = (int)activeParams.lfoType;
      v++;
      if (v > 3)
        v = 0;
      activeParams.lfoType = (LfoType)v;
      drawSliderControl(rx, ry, rw, rh, "LFO Type", (float)v, 0, 3, true);
      lastTopPress = millis();
    } break;
    case 3: // LDR Tgt
    {
      int v = (int)activeParams.ldrTarget;
      v++;
      if (v > 6)
        v = 0;
      activeParams.ldrTarget = (LfoTarget)v;
      drawSliderControl(rx, ry, rw, rh, "LDR Tgt", (float)v, 0, 6, true);
      lastTopPress = millis();
    } break;
    }
    // Update derived after change
    updateDerivedParameters();
    return; // Done for Top Row
  }

  // --- BOTTOM ROWS (Sliders) ---
  else {
    int r = (ty - topH) / botH; // 0, 1, 2
    int c = tx / botW;          // 0, 1, 2

    // Clamp
    if (r < 0)
      r = 0;
    if (r > 2)
      r = 2;
    if (c < 0)
      c = 0;
    if (c > 2)
      c = 2;

    int idx = r * 3 + c;

    rx = c * botW;
    ry = topH + r * botH;
    rw = botW;
    rh = botH;

    switch (idx) {
    case 0: // LFO Hz
      sliderTarget = &activeParams.lfoRate;
      minV = 0.125;
      maxV = 10.0;
      label = "LFO Hz";
      break;
    case 1: // LFO Depth
      sliderTarget = &activeParams.lfoDepth;
      minV = 0.0;
      maxV = 1.0;
      label = "LFO Depth";
      break;
    case 2: // Drive
      sliderTarget = &activeParams.driveAmount;
      minV = 0.05;
      maxV = 0.6;
      label = "Drive";
      break;
    case 3: // Cutoff
      sliderTarget = &activeParams.filterCutoff;
      minV = 100;
      maxV = 4000;
      label = "Cutoff";
      break;
    case 4: // Res
      sliderTarget = &activeParams.filterRes;
      minV = 0.01;
      maxV = 0.9;
      label = "Res";
      break;
    case 5: // Dly FB
      sliderTarget = &activeParams.delayFeedback;
      minV = 0.05;
      maxV = 0.7;
      label = "Dly FB";
      break;
    case 6: // Attack
      sliderTarget = &activeParams.attackTime;
      minV = 0.001;
      maxV = 0.150;
      label = "Attack";
      break;
    case 7: // Release
      sliderTarget = &activeParams.releaseTime;
      minV = 0.100;
      maxV = 3.000;
      label = "Release";
      break;
    case 8: // Trem Hz
      sliderTarget = &activeParams.tremRate;
      minV = 0.25;
      maxV = 5.0;
      label = "Trem Hz";
      break;
    }
  }

  // Handle Slider Logic
  if (sliderTarget) {
    // Calculate normalized value relative to THIS control's rect
    // tx is absolute scren coord. Need relative to rx.
    int relX = tx - rx;
    float norm = (float)relX / (float)rw;

    // Clamp norm (User: "Only slides right" -> logic error check)
    // If relX is negative, norm is negative.
    if (norm < 0.0f)
      norm = 0.0f;
    if (norm > 1.0f)
      norm = 1.0f;

    // Apply
    *sliderTarget = minV + norm * (maxV - minV);

    // Redraw
    drawSliderControl(rx, ry, rw, rh, label, *sliderTarget, minV, maxV, false);
    updateDerivedParameters();
  }
}

// --- BOOT UI ---
void drawBootScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);

  // Split Screen
  // Left: Bluetooth
  tft.fillRect(0, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT, TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("BLUETOOTH", SCREEN_WIDTH / 4, SCREEN_HEIGHT / 2);

  // Right: Speaker
  tft.fillRect(SCREEN_WIDTH / 2, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT,
               TFT_DARKGREEN);
  tft.drawString("SPEAKER", 3 * SCREEN_WIDTH / 4, SCREEN_HEIGHT / 2);

  tft.setTextSize(1);
  tft.drawString("Select Audio Output", SCREEN_WIDTH / 2, 20);
}

// Highlight Index: -1 for none
void drawBTSelectScreen(int highlightIdx = -1) {
  static bool firstRun = true;
  if (firstRun) {
    tft.fillScreen(TFT_BLACK);
    firstRun = false;
  }

  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  // grid 2x3
  int rows = 2;
  int cols = 3;
  int w = SCREEN_WIDTH / cols;
  int h = SCREEN_HEIGHT / rows;

  // Colors for slots 0-4 (Backgrounds)
  uint16_t slotBGs[] = {TFT_CYAN, TFT_YELLOW, TFT_MAGENTA, TFT_GREEN,
                        TFT_ORANGE};
  uint16_t slotTexts[] = {TFT_BLACK, TFT_BLACK, TFT_WHITE, TFT_BLACK,
                          TFT_BLACK};

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      int idx = r * cols + c;
      int x = c * w;
      int y = r * h;

      uint16_t bg, fg;

      // Slot 5 (Index 5) is REFRESH
      if (idx == 5) {
        bg = TFT_DARKGREY;
        fg = TFT_WHITE;
      } else {
        // Device Slot
        bg = (idx < 5) ? slotBGs[idx] : TFT_DARKGREY;
        fg = (idx < 5) ? slotTexts[idx] : TFT_WHITE;
        if (idx >= foundDeviceCount) {
          bg = TFT_BLACK; // Empty
          fg = TFT_WHITE;
        }
      }

      // Highlight Logic (Invert)
      if (idx == highlightIdx) {
        uint16_t temp = bg;
        bg = fg;
        fg = temp;
        // Ensure contrast if black/white swap happened poorly?
        if (bg == TFT_BLACK && fg == TFT_BLACK)
          fg = TFT_WHITE;
      }

      tft.fillRect(x, y, w, h, bg);
      tft.drawRect(x, y, w, h, TFT_WHITE);

      tft.setTextColor(fg);
      if (idx == 5) {
        tft.drawString("REFRESH", x + w / 2, y + h / 2);
        tft.drawString("(Scan Again)", x + w / 2, y + h / 2 + 15);
      } else {
        if (idx < foundDeviceCount) {
          tft.drawString(foundDevices[idx].name, x + w / 2, y + h / 2 - 10);
          char rssiStr[16];
          sprintf(rssiStr, "%d dBm", foundDevices[idx].rssi);
          tft.drawString(rssiStr, x + w / 2, y + h / 2 + 10);
        } else {
          tft.drawString("Scanning...", x + w / 2, y + h / 2);
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  SynthVoice::initLUT();

  // Initialize Wave Presets
  for (int i = 0; i < 4; i++) {
    wavePresets[i] = activeParams;
  }

  // Init Delay Buffer (Dynamic int16_t)
  if (delayBuffer == 0) {
    delayBuffer = (int16_t *)calloc(MAX_DELAY_LEN, sizeof(int16_t));
    if (delayBuffer == 0) {
      Serial.println("CRITICAL: Failed to allocate Delay Buffer!");
    } else {
      Serial.printf("Allocated Delay Buffer: %d bytes\n", MAX_DELAY_LEN * 2);
    }
  }

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COLOR_BG);

  // Init Touch (using global SPI, re-mapped)
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin();
  ts.setRotation(1);

  // Init Audio Frequencies
  float k = 1.059463094359;
  float f = 130.81f;
  for (int i = 0; i < STRING_COUNT; i++) {
    baseFreqs[i] = f;
    f *= k;
  }

  // Show Boot Selection
  delay(500); // Allow touch hardware to settle/debounce on power-up
  drawBootScreen();
}
// --- HELPER FUNCTION: Trigger Note ---
void triggerNote(int sIdx) {
  // Logic: Check Chord Mask & Inversion Cutoff
  int btnState = (activeButtonIndex >= 1 && activeButtonIndex <= 6)
                     ? buttonStates[activeButtonIndex]
                     : 0;
  int invCutoff = 0;
  if (btnState >= 2 && btnState <= 4) {
    invCutoff = getInversionCutoff(currentChordMask, btnState - 1);
  }

  // Check Mask
  if ((currentChordMask & (1 << (sIdx % 12))) && sIdx >= invCutoff) {

    // Calculate Frequency w/ Speaker Boost for Sine
    int extraOctave = 0;
    if (currentProfile == &spkProfile && currentWaveform == WAVE_SINE) {
      extraOctave = 2;
    }

    float freq = baseFreqs[sIdx];

    // Apply Global Octave Shift + Extra Boost
    int totalTwist = octaveShift + extraOctave;
    if (totalTwist > 0) {
      for (int k = 0; k < totalTwist; k++)
        freq *= 2.0f;
    } else if (totalTwist < 0) {
      for (int k = 0; k < abs(totalTwist); k++)
        freq *= 0.5f;
    }

    // Find Voice
    int vIdx = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
      if (!voices[v].active) {
        vIdx = v;
        break;
      }
    }
    if (vIdx == -1) // Steal voice 0 if all active
      vIdx = 0;

    voices[vIdx].trigger(freq, sIdx);
    stringEnergy[sIdx] = 1.0f;
    // Serial.printf("String %d Triggered (Freq: %.2f)\n", sIdx, freq);
  }
}

void loop() {
  // --- BOOT SELECTION MODE ---
  if (audioTarget == TARGET_BOOT) {
    // STARTUP DELAY LOCK & RELEASE GUARD
    // 1. Hard Wait: 1 second for power-on transients
    static uint32_t bootModeStart = 0;
    if (bootModeStart == 0)
      bootModeStart = millis();
    if (millis() - bootModeStart < 1000) {
      return;
    }

    // 2. Release Check: Must see "No Touch" before accepting anything
    // This handles cases where ghost touch persists > 1s or user holds screen
    static bool preReleaseDetected = false;
    if (!ts.touched()) {
      preReleaseDetected = true;
    }
    if (!preReleaseDetected) {
      // Still detecting boot touch/noise - wait until it clears
      return;
    }

    if (ts.touched()) {
      TS_Point p = ts.getPoint();

      // Increased Pressure Threshold (Ghost touch was ~690)
      if (p.z < 800) {
        return;
      }

      // Debounce: Verify touch is real and sustained
      delay(100);
      if (!ts.touched())
        return;

      TS_Point p2 = ts.getPoint(); // Read again
      if (abs(p.x - p2.x) > 500)
        return; // Jitter check

      // FIXED MAPPING: Aligned with Main Loop input (380->3700)
      int touchX = map(p.x, 380, 3700, 0, 320);
      Serial.printf("Valid Boot Touch: Raw X=%d Z=%d -> MapX=%d\n", p.x, p.z,
                    touchX);

      if (touchX < 160) {
        Serial.println("Selection: BLUETOOTH SELECT");
        audioTarget = TARGET_BT_SELECT;
        setupBluetooth(true); // Starts Scanning NO AUTO CONNECT
        drawBTSelectScreen(-1);
      } else {
        Serial.println("Selection: SPEAKER");
        audioTarget = TARGET_SPEAKER;
        setupSpeaker();
        // Refresh UI
        tft.fillScreen(COLOR_BG);
        drawInterface();
        delay(500); // Debounce
      }
    }
    return;
  }

  // --- BLUETOOTH SELECTION MODE ---
  if (audioTarget == TARGET_BT_SELECT) {
    static uint32_t lastRedraw = 0;

    // Auto-Redraw periodically to show new devices
    if (millis() - lastRedraw > 500) { // Faster 2Hz update
      if (!ts.touched())               // Don't redraw if user is touching
        drawBTSelectScreen(-1);
      lastRedraw = millis();
    }

    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      if (p.z > 400) {
        // Map Touch
        int touchX = map(p.x, 380, 3700, 0, 320);
        int touchY = map(p.y, 450, 3700, 0, 240);

        int cols = 3;
        int rows = 2;
        int col = touchX / (SCREEN_WIDTH / cols);
        int row = touchY / (SCREEN_HEIGHT / rows);
        int idx = row * cols + col;

        // Visual Feedback
        drawBTSelectScreen(idx);
        delay(150); // Short hold

        if (idx == 5) { // REFRESH
          Serial.println("Refreshing Scan...");
          foundDeviceCount = 0;
          targetSSID[0] = '\0';
          drawBTSelectScreen(idx); // Keep Highlight?
          a2dp_source.end();       // Stop
          delay(100);
          a2dp_source.start(); // Restart Scan
          a2dp_source.set_volume(100);
        } else {
          // DEVICE SELECT
          if (idx < foundDeviceCount) {
            Serial.printf("User Selected: %s\n", foundDevices[idx].name);
            strncpy(targetSSID, foundDevices[idx].name, 63);

            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(TFT_WHITE);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("Connecting to:", SCREEN_WIDTH / 2,
                           SCREEN_HEIGHT / 2 - 20);
            tft.drawString(targetSSID, SCREEN_WIDTH / 2,
                           SCREEN_HEIGHT / 2 + 10);

            // Enable re-connect now that we have a target
            a2dp_source.set_auto_reconnect(true);
            // Trigger connection?
            // The library cycle will pick it up in ssid_callback.
            // We might need to restart it to force callback check immediately
            // But callback runs on scan results.

            audioTarget = TARGET_BLUETOOTH;
            delay(1000);

            // Setup Main UI
            tft.fillScreen(COLOR_BG);
            drawInterface();
          }
        }
      }
    }
    return;
  }

  // --- RUNNING MODE ---

  // --- RUNNING MODE ---

  if (audioTarget == TARGET_SPEAKER) {
    fillAudioBuffer();
  }

  // --- EDITOR MODE ---
  if (currentMode == MODE_EDIT) {
    static uint32_t lastEditVis = 0;
    // Throttled Redraw or Logic?
    // Input is handled below.

    // Clear Block on Release with Debounce
    static uint32_t releaseDebounceStart = 0;

    if (!ts.touched()) {
      if (releaseDebounceStart == 0)
        releaseDebounceStart = millis();

      if (millis() - releaseDebounceStart > 50) {
        // Check for Piano/Exit Short Press Release
        if (editorPianoPressStart > 0) {
          // Check for Short Press Exit
          if (!editorPianoHandled && millis() - editorPianoPressStart < 700) {
            // Short Press -> Exit
            currentMode = MODE_PLAY;
            tft.fillScreen(COLOR_BG);
            drawInterface();
            delay(200);
            inputBlockTimer =
                millis() + 500; // Prevent ghost touches in Play Mode
          }
          editorPianoPressStart = 0;
        }
        editorInputBlocked = false;
      }
    } else {
      releaseDebounceStart = 0;
    }

    // Editor Input
    if (ts.touched()) {
      if (editorInputBlocked)
        return;

      TS_Point p = ts.getPoint();
      if (p.z > 400) {
        int tx = map(p.x, 380, 3700, 0, 320);
        int ty = map(p.y, 450, 3700, 0, 240);

        if (tx < 0)
          tx = 0;
        if (tx > 320)
          tx = 320;
        if (ty < 0)
          ty = 0;
        if (ty > 240)
          ty = 240;

        handleEditorTouch(tx, ty);
        // Debounce sliders slightly? No, smooth drag needed.
      }
    }
    return; // Skip normal UI
  }

  // Bluetooth Status Monitor
  if (audioTarget == TARGET_BLUETOOTH) {
    static uint32_t lastBtCheck = 0;
    if (millis() - lastBtCheck > 1000) {
      lastBtCheck = millis();
      if (!a2dp_source.is_connected()) {
        Serial.println("Bluetooth: Still disconnected... (Loop Alive)");
      } else {
        // Connected
        Serial.println("Bluetooth: Connected! (Loop Alive)");
      }
    }
  }

  // Update Visuals (Throttled to ~60FPS to save SPI/CPU)
  static uint32_t lastVis = 0;
  if (millis() - lastVis > 15) {
    updateStringVisuals();
    lastVis = millis();
  }

  // 2. Read LDR & Update Waveform Logic (Throttled to ~50Hz)
  static uint32_t lastLdr = 0;
  static int ldrVal = 0;       // Static for Logger visibility
  static float ldrNorm = 0.0f; // Static for Logger visibility

  if (millis() - lastLdr > 20) {
    ldrVal = analogRead(LDR_PIN);

    // Smoothing (Exponential Moving Average)
    static float smoothedLdr = 0;
    if (smoothedLdr == 0)
      smoothedLdr = ldrVal; // Init
    smoothedLdr = smoothedLdr * 0.9f + (float)ldrVal * 0.1f;

    // START of LOOP LOGIC
    // LDR Smoothing
    smoothedLdr = smoothedLdr * 0.9f + (float)ldrVal * 0.1f;
    int constrainedLdr = constrain((int)smoothedLdr, 0, 2000);

    // Global LFO is updated in Audio Thread for smoothness
    // We just read globalLfoVal for slow modulations (Attack)
    float lfoUnipolar = (globalLfoVal + 1.0f) * 0.5f;

    // Widen Range: 0 (Dark) to 2000 (Bright)
    // LDR Logic
    // Logic:
    // ldrVal: 0 (Bright) to 2000 (Dark)
    // norm:   1.0 (Bright) to 0.0  (Dark) if inverted?
    // User requested "Lowest freq when covered (Dark)".
    // So Dark (2000) -> 0.0 Norm.
    // Bright (0) -> 1.0 Norm.
    // map(val, 0, 2000, 1000, 0) / 1000.0f does exactly this.
    ldrNorm = map(constrainedLdr, 0, 2000, 1000, 0) / 1000.0f;

    // --- WAVEFORM SPECIFIC CONTROL LOGIC ---
    // REFACTOR: Use activeParams as BASE value, apply LDR as MODULATION.
    // LDR: 0 (Bright/Open) to 1 (Dark/Closed) -> ldrNorm is Inverted?
    // ldrNorm: 0.0 (Dark) to 1.0 (Light) was previous logic?
    // Constrained ldrNorm: 0.0 (Dark) to 1.0 (Light).

    // Logic:
    // Filter: Base = activeParams.filterCutoff.
    // LDR darkens filter? Or opens?
    // "Lowest freq when covered (Dark)". Light = Max (Base).

    float ldrMod = constrain(ldrNorm, 0.0f, 1.0f);

    // --- LDR MODULATION LOGIC ---
    // ldrNorm: 0.0 (Dark) to 1.0 (Light).
    // Logic: Slider Value sets the MAXIMUM (Light).
    //        LDR scales down to a Minimum (Dark).

    // float ldrMod = constrain(ldrNorm, 0.0f, 1.0f); // Duplicate, removed

    // Default Targets
    float targetCutoff = activeParams.filterCutoff; // Default to slider
    float targetRes = activeParams.filterRes;
    float targetFold = activeParams.waveFold;
    float targetPitch = 1.0f; // Multiplier? Or offset.
    // Pitch is handled in Audio for modulation, but Base Pitch is keyboard.
    // LDR Pitch usually means Bend? Or just Detune?
    // Let's make it +/- 1 Semitone range scaled by LDR?
    // Or scaling base freq?
    // Let's assume Pitch Target = Vibrato/Bend depth?
    // Or if User selects Pitch, LDR 0..1 maps to -12..+12 semitones? Too
    // extreme. Let's do: Dark = -5%, Light = +5%? Or just Light = Base, Dark =
    // -1 Octave? (Theremin style). Let's try: Dark = BaseFreq, Light = BaseFreq
    // * 2 (+1 Octave)? User didn't specify. Let's stick to subtle or Filter
    // logic. If Pitch selected: Dark=Normal, Light = +1 Octave (Theremin ish)
    // This requires updating 'pitchMod' global? Or voices?
    // Pitch mod in `generateMixedSample(pitchMod...)`.
    // We can update a global `ldrPitchMod` usage?
    // Currently `pitchMod` is calculated in `fillAudioBuffer` from LFO/Wobble.
    // We can add a global `float globalLdrPitchMod`?
    // Or we skip Pitch for LDR for now as it crosses thread domains dangerously
    // without atomic. Actually `fillAudioBuffer` reads `activeParams` anyway.
    // But LDR is updated in Loop.
    // Let's Skip Pitch for this iteration or map it to something safe.

    float targetAtk = activeParams.attackTime;
    float targetRel = activeParams.releaseTime;

    // Apply LDR Modulation to ONE target
    // Note: activeParams.ldrTarget is Enum
    if (activeParams.ldrTarget == TARGET_FILTER) {
      // Dark = 100Hz, Light = Slider
      float minF = 100.0f;
      targetCutoff = minF + (activeParams.filterCutoff - minF) *
                                (ldrMod * ldrMod); // Exp curve
    } else if (activeParams.ldrTarget == TARGET_RES) {
      // Dark = 0, Light = Slider
      targetRes = activeParams.filterRes * ldrMod;
    } else if (activeParams.ldrTarget == TARGET_FOLD) {
      // Dark = 0.1, Light = Slider
      float minFold = 0.1f;
      targetFold = minFold + (activeParams.waveFold - minFold) * ldrMod;
    } else if (activeParams.ldrTarget == TARGET_PITCH) {
      // Pitch Bend: Dark = -2 semitones, Light = +2?
      // Or 0..1 -> 0..+12?
      // Let's do Dark=Normal, Light = +1 Octave (Theremin ish)
      // This requires updating 'pitchMod' global? Or voices?
      // Pitch mod in `generateMixedSample(pitchMod...)`.
      // We can update a global `ldrPitchMod` usage?
      // Currently `pitchMod` is calculated in `fillAudioBuffer` from
      // LFO/Wobble. We can add a global `float globalLdrPitchMod`? Or we skip
      // Pitch for LDR for now as it crosses thread domains dangerously without
      // atomic. Actually `fillAudioBuffer` reads `activeParams` anyway. But LDR
      // is updated in Loop. Let's Skip Pitch for this iteration or map it to
      // something safe.
    } else if (activeParams.ldrTarget == TARGET_ATTACK) {
      // Dark = Fast (0), Light = Slider
      targetAtk = activeParams.attackTime * ldrMod;
      if (targetAtk < 0.001f)
        targetAtk = 0.001f;
    } else if (activeParams.ldrTarget == TARGET_RELEASE) {
      // Dark = Short (0.1), Light = Slider
      targetRel = 0.1f + (activeParams.releaseTime - 0.1f) * ldrMod;
    }

    // --- APPLY TO ENGINE ---

    // 1. Filter (SVF)
    // Smoothing handled here
    float new_f = 2.0f * sin(PI * targetCutoff / (float)SAMPLE_RATE);
    if (new_f > 0.9f)
      new_f = 0.9f;
    svf_f = svf_f * 0.9f + new_f * 0.1f;

    if (targetRes > 0.95f)
      targetRes = 0.95f;
    svf_q = 1.0f - targetRes;

    // 2. Waveform / PWM
    globalPulseWidth = targetFold; // For Square
    // For Saw/Tri, `pwMod` in audio thread handles Fold??
    // Wait, `fillAudioBuffer` used `activeParams.waveFold`.
    // If we want LDR to affect Fold, we must update `activeParams.waveFold`?
    // NO! That moves the Slider!
    // We need an intermediate "Effective Value".
    // But `fillAudioBuffer` reads `activeParams` directly.
    // Solution: `fillAudioBuffer` should read `effectiveParams`?
    // Or we use `globalPulseWidth` as the "Effective Fold" transfer variable?
    // Currently `globalPulseWidth` is used for Square.
    // Saw/Tri use `derivedPwMod` calculated from `activeParams.waveFold`.
    // We should update `globalPulseWidth` to represent the Effective Fold for
    // ALL waves? And update `fillAudioBuffer` to use `globalPulseWidth` instead
    // of `activeParams.waveFold`? Yes! `globalPulseWidth` is volatile float.
    // Safe-ish.

    // 3. Envelopes
    for (int i = 0; i < MAX_VOICES; i++) {
      voices[i].setADSR(targetAtk, 0.1f, 0.7f, targetRel);
      voices[i].setPulseWidth(globalPulseWidth);
      // Mix Gain? Keep profile?
      if (currentWaveform == WAVE_SAW)
        voices[i].mixGain = currentProfile->gainSaw;
      else if (currentWaveform == WAVE_SQUARE)
        voices[i].mixGain = currentProfile->gainSquare;
      else if (currentWaveform == WAVE_SINE)
        voices[i].mixGain = currentProfile->gainSine;
      else if (currentWaveform == WAVE_TRIANGLE)
        voices[i].mixGain = currentProfile->gainTri;
    }

    lastLdr = millis();
  }

  // Log LDR occasionally
  static uint32_t lastLdrLog = 0;
  if (millis() - lastLdrLog > 100) { // Faster Log (100ms)
    Serial.printf("Wave: %d, Raw: %d, Norm: %.2f, PW: %.2f, Wobble: %.4f\n",
                  currentWaveform, ldrVal, ldrNorm, globalPulseWidth,
                  wobbleDepth);
    lastLdrLog = millis();
  }

  // 3. Input Handling
  // Strategy: Call fillAudioBuffer() frequently.

  if (ts.touched()) {
    if (millis() < inputBlockTimer)
      return; // Block input during mode transitions
    // Fill buffer again before expensive touch logic (Only if Speaker)
    if (audioTarget == TARGET_SPEAKER) {
      fillAudioBuffer();
    }

    TS_Point p = ts.getPoint();

    // Z-Pressure Threshold
    if (p.z < 400) {
      if (lastTouchedString != -1) {
        // Release held note
        // Iterate through all voices to find and release the correct one
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
              voices[v].held) {
            voices[v].release();
          }
        }
      }
      lastTouchedString = -1;
      return; // Pressure too low
    }

    // Continue if pressure is good
    // } // Removed premature closing brace

    // Calibration (User Provided)
    int tx = map(p.x, 380, 3700, 0, 320);
    int ty = map(p.y, 450, 3700, 0, 240);

    // Bounds checking...
    if (tx < 0)
      tx = 0;
    if (tx > SCREEN_WIDTH)
      tx = SCREEN_WIDTH;
    if (ty < 0)
      ty = 0;
    if (ty > SCREEN_HEIGHT)
      ty = SCREEN_HEIGHT;

    // --- TOUCH DISPATCH LOGIC ---
    // 1. Top Zone: Sound Design (5 Buttons)
    if (ty < 50) {
      static uint32_t lastTopPress = 0;
      if (millis() - lastTopPress > 300) {
        // 1. Delay (0-64)
        // 1. Delay (0-64)
        if (tx < 64) {
          if (delayPressStart == 0)
            delayPressStart = millis();
        }
        // 2. Drive (64-128)
        else if (tx < 128) {
          fxDrive = !fxDrive;
          drawFXButtons();
        }
        // 3. Trem (128-192)
        else if (tx < 192) {
          fxTrem = !fxTrem;
          drawFXButtons();
        }
        // 4. LFO (192-256)
        else if (tx < 256) {
          fxLFO = !fxLFO;
          drawFXButtons();
        }
        // 5. Wave (>256)
        else {
          // Long Press Activity
          if (wavePressStart == 0)
            wavePressStart = millis();

          // Check Hold Duration
          if (wavePressStart > 0 && millis() - wavePressStart > 700) {
            // Enter Editor
            Serial.println("Entering Editor Mode");
            currentMode = MODE_EDIT;
            editorInputBlocked = true; // Block input until release
            tft.fillScreen(COLOR_BG);
            drawEditor();
            wavePressStart = 0; // Reset
            delay(500);         // Debounce prevent immediate exit
            return;
          }
          // Do NOT toggle here. Toggle on release.
        }
        lastTopPress = millis();
      } else {
        // Reset press start if we moved out of button?
        // No, this else block is unreachable because `if (tx < 256)` handles
        // 0-256. Wait, the logic is `if (millis() - lastTopPress > 300)`. If we
        // are holding, we enter this block repeatedly every 300ms. We only want
        // to set start time once. Logic fix: Move `wavePressStart` init OUTSIDE
        // the 300ms check? Or just check if it's 0 inside.
      }

      // Clear string interaction if touching buttons
      if (lastTouchedString != -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
              voices[v].held)
            voices[v].release();
        }
        lastTouchedString = -1;
      }

    }
    // RESET Wave Press if NOT touching top right?
    // Handled in global "else" block below.

    // 2. Bottom Zone: Performance Buttons (Start at 200)
    else if (ty > SCREEN_HEIGHT - 40) {
      int fw = SCREEN_WIDTH / 8;
      int btnIdx = tx / fw;
      if (btnIdx >= 0 && btnIdx < 8) {
        static uint32_t lastBtnPress = 0;
        if (millis() - lastBtnPress > 200) {
          handleButtonPress(btnIdx);
          lastBtnPress = millis();
        }
      }
      // Clear string interaction
      if (lastTouchedString != -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
              voices[v].held)
            voices[v].release();
        }
        lastTouchedString = -1;
      }
    }
    // 3. Slider Zone (170 - 200)
    else if (ty > SCREEN_HEIGHT - 70) {
      // Master Volume
      // Map tx 0..320 to 0.0..1.0
      // Clamp edges for safety
      float vol = (float)tx / (float)SCREEN_WIDTH;
      if (vol < 0.0f)
        vol = 0.0f;
      if (vol > 1.0f)
        vol = 1.0f;

      masterVolume = vol;
      drawVolumeSlider();

      // Clear string interaction
      if (lastTouchedString != -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
              voices[v].held)
            voices[v].release();
        }
        lastTouchedString = -1;
      }
    }
    // 4. Middle Zone: Strings (50 - 170)
    else {
      // Calculate String Index
      int sIdx = (tx * STRING_COUNT) / SCREEN_WIDTH;

      if (sIdx >= 0 && sIdx < STRING_COUNT) {
        if (sIdx != lastTouchedString) {
          // Release Old
          if (lastTouchedString != -1) {
            for (int v = 0; v < MAX_VOICES; v++) {
              if (voices[v].active &&
                  voices[v].noteIndex == lastTouchedString && voices[v].held)
                voices[v].release();
            }
          }

          // Trigger New
          triggerNote(sIdx);
          lastTouchedString = sIdx;
        }
      }
    }
  } // End if (ts.touched())

  // Cleanup if no touch
  else {
    // Check for Delay Toggle on Release
    if (delayPressStart > 0) {
      delayMode++;
      if (delayMode > 4)
        delayMode = 0;
      drawDelayButton();
      delayPressStart = 0;
    }

    // Check for Waveform Toggle on Release
    if (wavePressStart > 0) {
      uint32_t dur = millis() - wavePressStart;
      if (dur < 2000) { // Short press (held less than 2s)
        // TOGGLE WAVEFORM
        // TOGGLE WAVEFORM
        int next = currentWaveform;
        if (currentWaveform == WAVE_SAW)
          next = WAVE_SQUARE;
        else if (currentWaveform == WAVE_SQUARE)
          next = WAVE_SINE;
        else if (currentWaveform == WAVE_SINE)
          next = WAVE_TRIANGLE;
        else
          next = WAVE_SAW;

        selectWaveform(next);

        drawWaveButton();
        Serial.printf("Wave: %d\n", currentWaveform);
      }
      wavePressStart = 0; // Reset
    }

    if (lastTouchedString != -1) {
      for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
            voices[v].held) {
          voices[v].release();
        }
      }
      lastTouchedString = -1;
    }
  }

} // End loop()

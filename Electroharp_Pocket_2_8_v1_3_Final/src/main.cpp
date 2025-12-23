// --- ESP32 CYD Autoharp ---
#include "Config.h"
#include "Settings.h"
#include "SynthVoice.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <XPT2046_Touchscreen.h>
#include <esp_task_wdt.h> // WDT Control

// --- AUDIO CONFIG ---
// Both Included for Dynamic Switching
#include "BluetoothA2DPSource.h"
#include "soc/rtc_io_reg.h"
#include "soc/sens_reg.h"
#include <driver/dac.h>
BluetoothA2DPSource a2dp_source;
bool isBluetoothActive =
    false; // Track if BT was ever started to prevent crash on end()

// Wired Audio Globals
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

#define PI 3.1415926535897932384626433832795

// Forward Decls
int getVisualStringOffset();
int getGlobalNoteIndex(int sIdx);
int getGlobalNoteIndexSafe(int sIdx);
enum AudioTarget {
  TARGET_SPLASH,
  TARGET_BOOT,
  TARGET_SPEAKER,
  TARGET_BLUETOOTH,
  TARGET_BT_SELECT,
  TARGET_CONFIG,
  TARGET_CALIBRATION_1,
  TARGET_CALIBRATION_2,
  TARGET_AUDIO_CONFIG,
  TARGET_PANIC_CONFIRM
};

AudioTarget audioTarget = TARGET_SPLASH;

// --- Globals ---
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

SynthVoice voices[MAX_VOICES];
float baseFreqs[STRING_COUNT];
int octaveShift = 0;
int latchedOctaveShift = 0; // Locked octave for Drone/Arp Latch
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
uint32_t wavePressStart = 0; // Moved to Top for Scope
// int activeButtonIndex = -1; // Already defined elsewhere

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
    4              // Octave Range (1-8, Default 4)
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
volatile int activeSampleRate = SAMPLE_RATE; // Default to 22050

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
volatile float wowInc = 2.0f * PI * 2.0f / 22050.0f;
volatile float flutterInc = 2.0f * PI * 3.0f / 22050.0f;

// Sound Design Tools State
bool fxDrive = false;
bool fxTrem = false;
bool fxLFO = false;

// Tremolo State
float tremPhase = 0.0f;
volatile float tremInc = 2.0f * PI * 5.0f / 22050.0f; // Updated by Params

// Global LFO State
float globalLfoPhase = 0.0f;
volatile float globalLfoVal = 0.0f;
volatile float globalLfoInc =
    2.0f * PI * 0.35f / SAMPLE_RATE;   // Updated by Params
volatile float globalLfoDepth = 0.30f; // New Global for LDR Mod

// Master Volume (User Controlled)
float masterVolume = 0.8f;

// --- ARPEGGIATOR STATE ---
// ArpMode defined in Config.h
// ArpMode defined in Config.h
ArpMode arpMode = ARP_OFF;

bool arpLatch = false;
int arpStep = 0;
uint8_t activeNotes[12]; // Stores string indices (0-11) of current chord
int activeNoteCount = 0;
uint8_t
    randomSequence[12]; // Stores shuffled INDICES into activeNotes (0..Count-1)
uint32_t arpPressStart = 0; // For long press

// --- SPARKLE MODE STATE ---
int noteTriggerCounter = 0;
struct SparkleEvent {
  bool active;
  uint32_t triggerTime;
  float freq;
  int stringIdx;
  float releaseTime;
  float velocity;
};
#define MAX_SPARKS 32
SparkleEvent pendingSparks[MAX_SPARKS];

// --- AUDIO CONFIG DATA ---
struct AudioConfigPreset {
  const char *name;
  int i2s_num;
  int bck;
  int ws;
  int dout;
  int format; // 0=I2S_Norm, 1=LSB (PT8211), 2=DAC_INT
};

#define AUDIO_PRESET_COUNT 5
AudioConfigPreset audioPresets[AUDIO_PRESET_COUNT] = {
    {"Internal DAC", 0, 26, 25, 22,
     2}, // Mode 2 = DAC. Pins irrelevant but stored.
    {"MAX98357 Std", 0, 26, 25, 22, 0},
    {"PCM5102 Std", 0, 26, 25, 22, 0}, // Often same as MAX, but useful label
    {"Custom 1", 0, 26, 25, 33, 0},
    {"PT8211 LSB", 0, 26, 25, 22, 1}};

bool isAudioTestRunning = false;
uint32_t lastAudioTestClick = 0;
// We need a pointer to AudioOutputI2S if we were using the library from Plus.
// But V5 Drone uses 'driver/dac' and manual I2S/BT in callbacks. It handles I2S
// manually in 'bt_data_stream_callback'?? Wait, V5 Drone uses
// 'BluetoothA2DPSource' which calls 'bt_data_stream_callback'. WIRED Audio in
// V5 Drone uses 'fillAudioBuffer' and 'onTimer' writing to DAC directly (line
// 435). CRITICAL DIFFERENCE: V5 Drone is DAC ONLY by default? Line 435:
// SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG... It does NOT seem to support external
// I2S natively in its current state? I need to ADD I2S support if I strictly
// want to port the "Audio Config" feature which configures I2S pins. If V5
// Drone is DAC-only, then Audio Config is useless unless I backport the I2S
// driver logic too. The user asked to "port the entire Config menu". I should
// port the UI and Logic, but I might need to adapt the Audio Engine to support
// I2S if it doesn't. Let's implement the UI and the 'apply' logic, but I might
// need to swap the audio backend mechanism? Actually 'Plus' used
// 'AudioOutputI2S' object. 'Drone' uses 'onTimer' ISR for DAC. To support I2S
// in Drone, I'd need to add `i2s_driver_install` logic and send data via
// `i2s_write`. Detailed plan:
// 1. Add I2S support to 'fillAudioBuffer' or a new I2S task.
// Or just let the user "Selected" I2S, but if the engine doesn't support it, it
// won't work. But I should try to make it work. Let's stick to the UI/Settings
// port first. I will add the structs.

// Forward Declarations
void updateActiveNotes();
void fireArp(int octaveOffset = 0, bool latched = false);
void triggerNote(int sIdx);

// Delay State
#define MAX_DELAY_MS 1200
#define DELAY_DOWNSAMPLE 4
#define MAX_DELAY_LEN (int)(44100 * MAX_DELAY_MS / 1000 / DELAY_DOWNSAMPLE)
// Using int16_t for buffer to save RAM (26KB now, super safe) + Super Vintage
// Grit
int16_t delayBuffer[MAX_DELAY_LEN];
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

// --- SPARKLE HELPER ---
void scheduleSpark(uint32_t delayMs, float freq, int sIdx, float rel,
                   float vel) {
  for (int i = 0; i < MAX_SPARKS; i++) {
    if (!pendingSparks[i].active) {
      pendingSparks[i].active = true;
      pendingSparks[i].triggerTime = millis() + delayMs;
      pendingSparks[i].freq = freq;
      pendingSparks[i].stringIdx = sIdx;
      pendingSparks[i].releaseTime = rel;
      pendingSparks[i].velocity = vel;
      break;
    }
  }
}

// --- CONFIG HELPER ---
// Forward decl
void drawCalibrationScreen(int step);

// --- CALIBRATION UI ---

void setupSpeaker(); // Existing? No, check if defined.
// Existing setupSpeaker() might not exist in Drone? It seems Drone uses
// 'audioTarget' switch in setup/loop. I'll check setup() later.

void applyAudioPreset(int index) {
  // STUB: In V5 Drone, audio is currently fixed to DAC via onTimer.
  // To support I2S switching, we'd need to overhaul the audio engine.
  // For now, we save the preference, but the engine ignores it (or we implement
  // later). We can at least print it.
  if (index >= 0 && index < AUDIO_PRESET_COUNT) {
    Serial.printf(
        "Applying Audio Preset: %s (Not fully supported in Drone Engine yet)\n",
        audioPresets[index].name);
  }
}

// Update Derived Parameters (Call after changing activeParams)
void updateDerivedParameters() {
  // Update Filter
  // svf_f = 2 * sin(PI * Fc / Fs)
  svf_f = 2.0f * sin(PI * activeParams.filterCutoff / (float)SAMPLE_RATE);
  if (svf_f > 0.95f)
    svf_f = 0.95f;

  // Update Resonance (q)
  // q = 1.0 - res. High Res = Low q.
  // Clamp Res to 0.95 to avoid explosion
  if (activeParams.filterRes > 0.95f)
    activeParams.filterRes = 0.95f;
  svf_q = 1.0f - activeParams.filterRes;

  // Update Rates
  globalLfoInc = 2.0f * PI * activeParams.lfoRate / (float)activeSampleRate;
  tremInc = 2.0f * PI * activeParams.tremRate / (float)activeSampleRate;
  wowInc = 2.0f * PI * 2.0f / (float)activeSampleRate;
  flutterInc = 2.0f * PI * 3.0f / (float)activeSampleRate;

  // Update Voice Envelopes (Global update for simplicity)
  for (int i = 0; i < MAX_VOICES; i++) {
    voices[i].attackTime = activeParams.attackTime;
    voices[i].releaseTime = activeParams.releaseTime;
    // Re-calc rates
    voices[i].attackRate = 1.0f / (activeParams.attackTime * activeSampleRate);
    voices[i].releaseRate =
        1.0f / (activeParams.releaseTime * activeSampleRate);
    voices[i].setWaveform(currentWaveform); // Fix visual only switching
  }
}

// --- PERFORMANCE GOVERNOR ---
// Start conservative to prevent WDT on first BT frame
int maxPolyphony = 6;
float cpuLoad = 0.0f; // 0.0 to 1.0 (Target < 0.8)
uint32_t lastLoadCheck = 0;

// Strum Rate Detection
uint32_t noteTriggerTimes[5] = {0};
int noteTriggerHead = 0;

bool detectFastStrum() {
  // Check span of last 5 notes
  uint32_t minT = 0xFFFFFFFF;
  uint32_t maxT = 0;
  int count = 0;
  for (int i = 0; i < 5; i++) {
    if (noteTriggerTimes[i] > 0) {
      if (noteTriggerTimes[i] < minT)
        minT = noteTriggerTimes[i];
      if (noteTriggerTimes[i] > maxT)
        maxT = noteTriggerTimes[i];
      count++;
    }
  }
  if (count < 4)
    return false;

  // If 4+ notes occurred within 200ms (50ms avg interval), that's a fast strum
  return (maxT - minT) < 200;
}

// --- AUDIO GENERATION LOGIC (Shared) ---
// Returns a single float sample (-1.0 to 1.0) mixed from all voices + Filtered
float generateMixedSample(float pitchMod, float pwMod, float filterMod,
                          float resMod) {
  float mixedSample = 0.0f;
  int activeCount = 0;

  for (int i = 0; i < MAX_VOICES; i++) {
    // Governor check: Early exit if we exceed dynamic polyphony
    // Simple approach: only render first 'maxPolyphony' active voices?
    // Better: Render all, but 'trigger' logic should limit creation.
    // Here we just render what is active.
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
  if (f > 0.85f)
    f = 0.85f; // More conservative limit for stability
  if (f < 0.005f)
    f = 0.005f;

  svf_low += f * svf_band;

  // Waveform-dependent Resonance Tuning
  float baseRes = activeParams.filterRes * resMod;
  if (baseRes > 0.95f)
    baseRes = 0.95f;
  if (baseRes < 0.01f)
    baseRes = 0.01f;

  // Convert Res to Q (damping). Low Damping = High Res.
  float q = 1.0f - baseRes;

  // --- v3.5 Q-Compensation Logic ---
  // Waveform-dependent Resonance Tuning
  if (currentWaveform == WAVE_SAW) {
    q = q * 2.0f;
  } else if (currentWaveform == WAVE_SINE) {
    q = q * 3.0f;
  }

  // Profile Specific Safety
  if (currentProfile == &spkProfile) {
    q = q * 1.2f;
  } else if (currentProfile == &btProfile) {
    // Bluetooth: Extra heavy damping to prevent "explosions"
    // Apply global boost to damping (Increased +25% per request)
    q = q * 1.75f;

    // Waveform specific extra protection for BT
    if (currentWaveform == WAVE_SINE) {
      q = q * 1.5f; // Multiplicative stack
    } else if (currentWaveform == WAVE_SQUARE) {
      q = q * 1.5f;
    } else if (currentWaveform == WAVE_TRIANGLE) {
      q = q * 1.2f;
    }
  }

  if (q > 1.0f)
    q = 1.0f;

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
// --- BLUETOOTH CALLBACK (Always Compile) ---
// The A2DP library calls this to get data.
// Signature match: int32_t (*)(Frame *data, int32_t len) where len is frame
// count
int32_t bt_data_stream_callback(Frame *data, int32_t len) {
  // START PROFILE
  uint32_t startT = micros();

  if (len <= 0 || delayBuffer == NULL)
    return len; // Safety Check

  // --- STARTUP THROTTLE ---
  // If not fully connected, output silence and return immediately.
  // This gives the generic BT stack maximum CPU to finish the handshake.
  if (!a2dp_source.is_connected()) {
    memset(data, 0, len * sizeof(Frame)); // Silence
    return len;
  }

  // --- OPTIMIZATION: Idle Silence ---
  // If no notes are playing and no delay lines are active, output silence.
  bool anyActive = false;
  for (int k = 0; k < MAX_VOICES; k++) {
    if (voices[k].active) {
      anyActive = true;
      break;
    }
  }

  if (!anyActive && delayMode == 0) {
    memset(data, 0, len * sizeof(Frame)); // Hardware Silence
    // Still run Governor (rarely) to keep state valid? Or just return?
    // Return early to save MAX CPU.
    // Set minimal load to allow Polyphony to recover
    cpuLoad = 0.0f;
    if (maxPolyphony < 24)
      maxPolyphony++; // Recover voices
    return len;
  }

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
    float wobble = (sin(wowPhase) + sin(flutterPhase)) * 0.0015f;

    // --- Modulators ---
    float pitchMod = 1.0f + wobble;
    float pwMod = 0.0f;
    float filterMod = 1.0f;
    float resMod = 1.0f;

    if (fxLFO) {
      float depth = globalLfoDepth; // Using Global Depth
      if (activeParams.lfoTarget == TARGET_PITCH) {
        pitchMod += lfoVal * depth * 0.1f;
      } else if (activeParams.lfoTarget == TARGET_FOLD) {
        pwMod += lfoVal * depth * 0.4f;
      } else if (activeParams.lfoTarget == TARGET_FILTER) {
        filterMod += lfoVal * depth * 0.8f;
      } else if (activeParams.lfoTarget == TARGET_RES) {
        resMod += lfoVal * depth * 0.5f;
      }
    }

    // Wave/Fold Parameter Mapping
    float totalPW = globalPulseWidth;
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
      int delaySamples = (activeSampleRate / DELAY_DOWNSAMPLE * delayMs) / 1000;
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

    // --- Audio Test Tone (BT) ---
    // --- Audio Test Tone (BT) ---
    // DISABLED: To save CPU and prevent WDT
    /*
    if (isAudioTestRunning) {
      static float testPhaseBT = 0;
      testPhaseBT += 2.0f * PI * 440.0f / 44100.0f;
      if (testPhaseBT >= 2.0f * PI)
        testPhaseBT -= 2.0f * PI;
      sample += sin(testPhaseBT) * 0.3f;
    }
    */

    // Clip hard
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;

    // Final Volume Reduction for Bluetooth (65% of max)
    sample *= 0.65f;

    // Convert to 16-bit
    int16_t out = (int16_t)(sample * 30000.0f);

    // Stereo Frame
    data[i].channel1 = out; // Left
    data[i].channel2 = out; // Right
  }

  // --- GOVERNOR LOGIC (Decimated) ---
  // Only run logic every 4th callback to save analysis CPU
  static int govSkip = 0;
  if (++govSkip >= 4) {
    govSkip = 0;
    // BT callback shouldn't take > 80% of its budget.
    uint32_t elapsed = micros() - startT;
    float budget = (float)len * (1000000.0f / 44100.0f); // 44.1k budget
    float load = (float)elapsed / budget;

    // Smoothing
    cpuLoad = cpuLoad * 0.8f + load * 0.2f;

    // Granular Governor (User Requested Table)
    int targetPoly = 18;
    if (cpuLoad < 0.50f)
      targetPoly = 18;
    else if (cpuLoad < 0.55f)
      targetPoly = 16;
    else if (cpuLoad < 0.60f)
      targetPoly = 14;
    else if (cpuLoad < 0.65f)
      targetPoly = 12;
    else if (cpuLoad < 0.70f)
      targetPoly = 10;
    else if (cpuLoad < 0.75f)
      targetPoly = 8;
    else if (cpuLoad < 0.80f)
      targetPoly = 6;
    else
      targetPoly = 4; // > 80% Emergency

    // Strum Rate Intervention
    if (detectFastStrum()) {
      // Fast strum detected! Anticipate load spike.
      // Cap at 12 to prevent buffer overrun during rapid allocation
      if (targetPoly > 12)
        targetPoly = 12;
    }

    maxPolyphony = targetPoly;
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

    // Direct Register Write (Fastest) for jitter reduction
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
// --- Audio Generation Task (Called in loop) ---
void fillAudioBuffer() {
  // START PROFILE
  uint32_t startT = micros();
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
    float wobble = (sin(wowPhase) + sin(flutterPhase)) * 0.0015f;

    // --- Modulators ---
    float pitchMod = 1.0f + wobble;
    float pwMod = 0.0f;
    float filterMod = 1.0f;
    float resMod = 1.0f;

    // --- LFO Routing ---
    if (fxLFO) {
      float depth = globalLfoDepth;
      if (activeParams.lfoTarget == TARGET_PITCH) {
        pitchMod += lfoVal * depth * 0.1f;
      } else if (activeParams.lfoTarget == TARGET_FOLD) {
        pwMod += lfoVal * depth * 0.4f;
      } else if (activeParams.lfoTarget == TARGET_FILTER) {
        filterMod += lfoVal * depth * 0.8f;
      } else if (activeParams.lfoTarget == TARGET_RES) {
        resMod += lfoVal * depth * 0.5f;
      }
    }

    float totalPW = globalPulseWidth;
    float derivedPwMod = (totalPW - 0.5f) + pwMod;
    float s = generateMixedSample(pitchMod, derivedPwMod, filterMod, resMod);

    // FX: Drive
    if (fxDrive) {
      float drive = 1.0f + activeParams.driveAmount * 3.0f;
      s *= drive;
      if (s > 1.2f)
        s = 1.2f;
      if (s < -1.2f)
        s = -1.2f;
      s = s - (s * s * s) * 0.333f;
    }

    // Delay Processing
    delayTick++;
    if (delayTick >= DELAY_DOWNSAMPLE)
      delayTick = 0;

    float delayed = 0.0f;
    if (delayMode > 0 && delayBuffer != 0) {
      int delayMs = delayMode * 300;
      int delaySamples = (activeSampleRate / DELAY_DOWNSAMPLE * delayMs) / 1000;
      int readPos = (delayHead - delaySamples + MAX_DELAY_LEN) % MAX_DELAY_LEN;
      delayed = (float)delayBuffer[readPos] * 3.3333e-5f;
    }

    float dry = s;
    s = dry + delayed * 0.5f;

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

  // --- GOVERNOR LOGIC ---
  // 256 samples @ 22-44k.
  uint32_t elapsed = micros() - startT;
  float budget = (256.0f * 1000000.0f) / activeSampleRate;
  float load = (float)elapsed / budget;

  // Smoothing
  cpuLoad = cpuLoad * 0.9f + load * 0.1f;

  if (millis() - lastLoadCheck > 50) { // Fast check (50ms)
    // Granular Governor
    int targetPoly = 18;
    if (cpuLoad < 0.50f)
      targetPoly = 18;
    else if (cpuLoad < 0.55f)
      targetPoly = 16;
    else if (cpuLoad < 0.60f)
      targetPoly = 14;
    else if (cpuLoad < 0.65f)
      targetPoly = 12;
    else if (cpuLoad < 0.70f)
      targetPoly = 10;
    else if (cpuLoad < 0.75f)
      targetPoly = 8;
    else if (cpuLoad < 0.80f)
      targetPoly = 6;
    else
      targetPoly = 4; // > 80% Emergency

    // Strum Rate Intervention
    if (detectFastStrum()) {
      if (targetPoly > 12)
        targetPoly = 12;
    }

    maxPolyphony = targetPoly;
    lastLoadCheck = millis();
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

// Transpose State
int rootNote = 0; // 0=C, 1=C#, etc.

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

// Helper for Yellow->Red Gradient (0..duration ms)
uint16_t getGradientColor(uint32_t start, uint32_t duration) {
  uint32_t elapsed = millis() - start;
  if (elapsed > duration)
    elapsed = duration;

  float ratio = (float)elapsed / (float)duration;
  // Yellow (255,255,0) -> Red (255,0,0)
  // R stays 255. G goes 255 -> 0. B stays 0.

  int g = (int)(255.0f * (1.0f - ratio));
  if (g < 0)
    g = 0;

  // Return RGB565 (R=31, G=g>>2, B=0)
  // R(5): 31
  // G(6): g >> 2 (63 max) -> 255->63. (255 * 63 / 255)
  // Manual macro:
  int r5 = 31;
  int g6 = (g * 63) / 255;
  int b5 = 0;

  return (r5 << 11) | (g6 << 5) | b5;
}

bool isBlackKey(int index) {
  // Shift pattern based on Root Note
  // String 0 plays Root Note.
  // Standard (C): 0=C (W), 1=C# (B), 2=D (W)...
  // Transpose (e.g. F#, note 6):
  // We want String 0 to look like ROOT.
  // In C Major, Root is White.
  // Wait, keyboard pattern is fixed: White Black White Black White White
  // Black... User wants: "change which keyboard notes are labeled to always
  // show the root note on the keyboard". Actually, "change which keyboard notes
  // are labeled" implies the PATTERN shifts? "When the scale is transposed,
  // change which keyboard notes are labeled to always show the root note on the
  // keyboard." If I play F# Major, String 0 is F#. F# is Black. Should String 0
  // become "Black"? Yes, if we want the physical keyboard to represent absolute
  // pitch. So: Absolute Pitch = (index + rootNote) % 12.
  int absolutePitch = (index + rootNote) % 12;
  return (absolutePitch == 1 || absolutePitch == 3 || absolutePitch == 6 ||
          absolutePitch == 8 || absolutePitch == 10);
}

void updateStringVisuals() {
  tft.setTextDatum(MC_DATUM);

  int numStrings = activeParams.octaveRange * 12 + 1;
  if (numStrings > STRING_COUNT)
    numStrings = STRING_COUNT;

  bool circlesNeedRedraw = false;
  for (int i = 0; i < numStrings; i++) {
    // 1. Calculate X position (Center of the 2px string)
    int centerX =
        (i * SCREEN_WIDTH) / numStrings + (SCREEN_WIDTH / (numStrings * 2));

    // 2. Determine index and decay energy
    int globalIdx = getGlobalNoteIndex(i);
    bool outOfBounds = (globalIdx < 0 || globalIdx >= STRING_COUNT);
    int clampedIdx = constrain(globalIdx, 0, STRING_COUNT - 1);

    if (stringEnergy[clampedIdx] > 0.01f) {
      stringEnergy[clampedIdx] *= 0.94f;
    } else {
      stringEnergy[clampedIdx] = 0.0f;
    }

    // 3. Determine Color
    bool isBlack = isBlackKey(clampedIdx % 12);
    uint16_t baseColor = isBlack ? COLOR_STRING_BLACK : COLOR_STRING_WHITE;
    if (outOfBounds)
      baseColor = alphaBlend(TFT_BLACK, baseColor, 0.3f);

    uint16_t activeColor = TFT_RED;

    uint16_t currentColor =
        stringEnergy[clampedIdx] > 0
            ? alphaBlend(baseColor, activeColor, stringEnergy[clampedIdx])
            : baseColor;

    // 4. Draw ONLY if color changed
    if (currentColor != lastStringColor[i]) {
      int drawY = 60;
      int drawH = 140 - drawY;

      // Calculate the zone for this string to clear negative space
      int xStart = (i * SCREEN_WIDTH) / numStrings;
      int xEnd = ((i + 1) * SCREEN_WIDTH) / numStrings;
      int zoneW = xEnd - xStart;

      // Fill background (Negative Space)
      tft.fillRect(xStart, drawY, zoneW, drawH, TFT_BLACK);

      // Draw 2px String
      tft.fillRect(centerX - 1, drawY, 2, drawH, currentColor);

      lastStringColor[i] = currentColor;
      circlesNeedRedraw = true;
    }
  }

  // Redraw labels on TOP to prevent clipping
  if (circlesNeedRedraw) {
    for (int i = 0; i < numStrings; i++) {
      int globalIdx = getGlobalNoteIndex(i);
      // Fixed logic: Label the Root Note only if within legal bounds
      if (globalIdx >= 0 && globalIdx < STRING_COUNT && (globalIdx % 12 == 0)) {
        int centerX =
            (i * SCREEN_WIDTH) / numStrings + (SCREEN_WIDTH / (numStrings * 2));
        int circleY = 60 + 12;
        int radius = 10;
        uint16_t color = lastStringColor[i];

        tft.fillCircle(centerX, circleY, radius, color);
        tft.drawCircle(centerX, circleY, radius, TFT_DARKGREY);
        tft.setTextColor(TFT_BLACK);
        tft.setTextSize(1);

        int noteNum = (globalIdx + rootNote) % 12;
        if (noteNum < 0)
          noteNum += 12;
        tft.drawString(noteNames[noteNum], centerX, circleY);
      }
    }
  }
}

// --- DRAW STRINGS (Full Redraw) ---
void drawStrings() {
  int numStrings = activeParams.octaveRange * 12 + 1;
  int offset = getVisualStringOffset();

  // Clear background for range expansion/reduction
  tft.fillRect(0, 60, SCREEN_WIDTH, 80, TFT_BLACK);

  for (int i = 0; i < STRING_COUNT; i++)
    lastStringColor[i] = 0x1234;

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

void shuffleArpSequence() {
  if (activeNoteCount <= 0)
    return;
  for (int i = 0; i < 12; i++)
    randomSequence[i] = i; // Reset
  for (int i = activeNoteCount - 1; i > 0; i--) {
    int j = random(i + 1);
    uint8_t temp = randomSequence[i];
    randomSequence[i] = randomSequence[j];
    randomSequence[j] = temp;
  }
}

void updateActiveNotes() {
  activeNoteCount = 0;
  // Determine inversion cutoff based on active button
  int btnState = (activeButtonIndex >= 1 && activeButtonIndex <= 6)
                     ? buttonStates[activeButtonIndex]
                     : 0;
  int invCutoff = 0;
  if (activeButtonIndex >= 1 && activeButtonIndex <= 6 && btnState >= 2 &&
      btnState <= 4) {
    invCutoff = getInversionCutoff(currentChordMask, btnState - 1);
  }

  for (int i = 0; i < 12; i++) {
    // Must be in mask AND above inversion cutoff
    if ((currentChordMask & (1 << i)) && (i >= invCutoff)) {
      activeNotes[activeNoteCount++] = i;
    }
  }

  if (arpStep >= activeNoteCount)
    arpStep = 0;

  // Re-init Random Sequence indices
  for (int i = 0; i < 12; i++)
    randomSequence[i] = i;

  if (arpMode == ARP_RANDOM && activeNoteCount > 0) {
    shuffleArpSequence();
  }
}

// --- UI HELPERS ---
// Layout: 6 Buttons across top (320px / 6 = 53px each)
// [Delay] [Drive] [Trem] [LFO] [Arp] [Wave]

void drawArpButton() {
  int x = 212; // Slot 4
  int y = 5;
  int w = 53;
  int h = 55; // Taller

  uint16_t color = TFT_BLACK;
  bool active = (arpMode != ARP_OFF);

  // Determine Color
  if (arpLatch) {
    color = TFT_YELLOW; // Latch Override
  } else if (active) {
    // Mode Colors?
    switch (arpMode) {
    case ARP_UP:
      color = TFT_GREEN;
      break;
    case ARP_DOWN:
      color = TFT_RED;
      break;
    case ARP_UPDOWN:
      color = TFT_MAGENTA;
      break;
    case ARP_WALK:
      color = TFT_ORANGE;
      break;
    case ARP_RANDOM:
      color = TFT_BLUE;
      break;
    case ARP_SPARKLE:
      color = TFT_CYAN;
      break;
    case ARP_SPARKLE2:
      color = TFT_CYAN;
      break;
    default:
      color = TFT_DARKGREY;
      break;
    }
  }

  tft.fillRect(x, y, w, h, color);
  tft.drawRect(x, y, w, h, TFT_WHITE);

  tft.setTextColor(active ? TFT_WHITE : TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  // Label: Mode Name or "Arp"
  const char *label = "Arp";
  if (arpLatch)
    label = "Latch";
  else if (arpMode == ARP_UP)
    label = "Up";
  else if (arpMode == ARP_DOWN)
    label = "Down";
  else if (arpMode == ARP_UPDOWN)
    label = "U/D";
  else if (arpMode == ARP_WALK)
    label = "Walk";
  else if (arpMode == ARP_RANDOM)
    label = "Rnd";
  else if (arpMode == ARP_SPARKLE)
    label = "SPKL";
  else if (arpMode == ARP_SPARKLE2)
    label = "SPK2";
  else if (arpMode == ARP_SPARKLE2)
    label = "SPK2";
  else
    label = "Arp";

  tft.drawString(label, x + w / 2, y + h / 2);
}

void drawWaveButton() {
  int x = 265; // Slot 5
  int y = 5;
  int w = 53;
  int h = 55; // Taller

  uint16_t bg = TFT_BLACK;
  if (currentWaveform == WAVE_SAW)
    bg = TFT_RED;
  else if (currentWaveform == WAVE_SQUARE)
    bg = TFT_DARKGREEN;
  else if (currentWaveform == WAVE_SINE)
    bg = TFT_BLUE;
  else if (currentWaveform == WAVE_TRIANGLE)
    bg = TFT_MAGENTA;
  else
    bg = TFT_BLACK;

  // Gradient Override (Hold Feedback)
  if (wavePressStart > 0) {
    bg = getGradientColor(wavePressStart, 250);
  }

  tft.fillRect(x, y, w, h, bg);
  tft.drawRect(x, y, w, h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE); // Always White Icon
  tft.setTextDatum(MC_DATUM);

  // Draw Icon (Centered in 53px width)
  int x0 = x + 2; // Padding
  // Icon width approx 48px
  int x1 = x0 + 12; // Peak 1
  int x2 = x0 + 24; // End Cycle 1
  int x3 = x0 + 36; // Peak 2
  int x4 = x0 + 48; // End Cycle 2

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
    // Square Line: |_|-|_|
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
    // Sine Wave Icon
    int prevY = yMid;
    for (int i = 0; i <= 48; i++) {
      float angle = (i / 48.0f) * 6.28318f; // 2PI
      int newY = yMid - (int)(sin(angle) * 10.0f);
      if (i > 0)
        tft.drawLine(x0 + i - 1, prevY, x0 + i, newY, TFT_WHITE);
      prevY = newY;
    }
  } else if (currentWaveform == WAVE_TRIANGLE) {
    // Triangle: /\/\ (2 Cycles)
    tft.drawLine(x0, yBot, x1, yTop, TFT_WHITE); // Up 1
    tft.drawLine(x1, yTop, x2, yBot, TFT_WHITE); // Down 1
    tft.drawLine(x2, yBot, x3, yTop, TFT_WHITE); // Up 2
    tft.drawLine(x3, yTop, x4, yBot, TFT_WHITE); // Down 2
  }
}

void drawDelayButton() {
  int x = 0; // Slot 0
  int y = 5;
  int w = 53;
  int h = 55; // Taller

  uint16_t color = TFT_BLACK; // Default Inactive
  const char *label = "Delay";

  if (delayMode == 0) {
    color = TFT_BLACK; // Off state (White Outline)
    label = "Dly";
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

  // Text Always White (User Request)
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);

  tft.setTextSize(1);
  tft.drawString(label, x + (w / 2), y + (h / 2));
}

void drawFXButtons() {
  int y = 5;
  int w = 53;
  int h = 55; // Taller

  // Drive (Btn 1) -> x=53
  int xDrive = 53;
  uint16_t cDrive = fxDrive ? TFT_RED : TFT_BLACK;
  tft.fillRect(xDrive, y, w, h, cDrive);
  tft.drawRect(xDrive, y, w, h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Drv", xDrive + w / 2, y + h / 2);

  // Trem (Btn 2) -> x=106
  int xTrem = 106;
  uint16_t cTrem = fxTrem ? TFT_ORANGE : TFT_BLACK;
  tft.fillRect(xTrem, y, w, h, cTrem);
  tft.drawRect(xTrem, y, w, h, TFT_WHITE);
  tft.drawString("Trm", xTrem + w / 2, y + h / 2);

  // LFO (Btn 3) -> x=159
  int xLFO = 159;
  uint16_t cLFO = fxLFO ? TFT_MAGENTA : TFT_BLACK;
  tft.fillRect(xLFO, y, w, h, cLFO);
  tft.drawRect(xLFO, y, w, h, TFT_WHITE);
  tft.drawString("LFO", xLFO + w / 2, y + h / 2);
}

// 240px Width for Volume (Left side)
void drawVolumeSlider() {
  int y = 140; // Shifted UP by 20px
  int h = 50;  // Taller by 10px
  int w = 240; // Reduced from 320

  // Clear Area
  tft.fillRect(0, y, w, h, TFT_BLACK);

  // Draw Bar Container
  tft.drawRect(0, y, w, h, TFT_DARKGREY);

  // Draw Filled Bar
  int fillW = (int)(masterVolume * w);
  if (fillW > w)
    fillW = w;
  tft.fillRect(0, y, fillW, h, TFT_CYAN);

  // "Volume" Label
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(ML_DATUM); // Middle Left
  tft.drawString("Volume", 10, y + h / 2);
}

void drawTransposeButton() {
  int x = 240;
  int y = 140; // Shifted UP by 20px
  int w = 80;
  int h = 50; // Taller by 10px

  bool isTransposed = (rootNote != 0);
  uint16_t color = isTransposed ? TFT_MAGENTA : TFT_BLACK;
  uint16_t textColor = TFT_WHITE;

  tft.fillRect(x, y, w, h, color);
  tft.drawRect(x, y, w, h, TFT_WHITE);

  tft.setTextColor(textColor);
  tft.setTextDatum(MC_DATUM);

  tft.drawString("Transpose", x + w / 2, y + 15); // Adjusted text Y
  tft.drawString(
      noteNames[rootNote % 12 < 0 ? rootNote % 12 + 12 : rootNote % 12],
      x + w / 2, y + 33); // Adjusted text Y
}

void drawCalibrationScreen(int step) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);

  if (step == 1) {
    tft.drawString("Touch Top Left", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
    // Draw Target
    tft.fillCircle(20, 20, 10, TFT_RED);
    tft.drawCircle(20, 20, 15, TFT_WHITE);
  } else if (step == 2) {
    tft.drawString("Touch Bottom Right", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
    // Draw Target
    tft.fillCircle(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, 10, TFT_RED);
    tft.drawCircle(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, 15, TFT_WHITE);
  } else if (step == 3) {
    tft.drawString("Calibration Done!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
    tft.setTextSize(1);
    tft.drawString("Saving settings...", SCREEN_WIDTH / 2,
                   SCREEN_HEIGHT / 2 + 30);
  }
}

void drawConfigMenu() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Configuration", SCREEN_WIDTH / 2, 20);

  // Buttons
  int btnY = 60;
  int btnH = 45;
  int btnW = 200;
  int btnX = (SCREEN_WIDTH - btnW) / 2;
  int startY = btnY; // Fix missing var
  int gap = 15;

  // 1. Audio Config
  tft.fillRect(btnX, startY, btnW, btnH, TFT_DARKGREEN);
  tft.drawRect(btnX, startY, btnW, btnH, TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("AUDIO CONFIG", btnX + btnW / 2, startY + btnH / 2);

  // 2. Calibrate
  tft.fillRect(btnX, startY + (btnH + gap), btnW, btnH, TFT_MAGENTA);
  tft.drawRect(btnX, startY + (btnH + gap), btnW, btnH, TFT_WHITE);
  tft.drawString("CALIBRATE", btnX + btnW / 2,
                 startY + (btnH + gap) + btnH / 2);

  // 3. Back Btn
  tft.fillRect(btnX, startY + 2 * (btnH + gap), btnW, btnH, TFT_RED);
  tft.drawRect(btnX, startY + 2 * (btnH + gap), btnW, btnH, TFT_WHITE);
  tft.drawString("EXIT", btnX + btnW / 2, startY + 2 * (btnH + gap) + btnH / 2);
}

void drawSplashScreen() {
  tft.fillScreen(TFT_BLACK);
  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;

  // Draw Electric Radiating Lines
  for (int i = 0; i < 20; i++) {
    uint16_t color = (i % 3 == 0) ? TFT_CYAN : (i % 3 == 1 ? 0x9E7D : 0xFFF0);
    int tx = random(0, SCREEN_WIDTH);
    int ty = random(0, SCREEN_HEIGHT);
    if (random(2) == 0)
      tx = (random(2) == 0) ? 0 : SCREEN_WIDTH;
    else
      ty = (random(2) == 0) ? 0 : SCREEN_HEIGHT;

    // Zig-zag line
    int lx = cx;
    int ly = cy;
    for (int j = 1; j <= 3; j++) {
      int nx = cx + (tx - cx) * j / 3 + random(-15, 15);
      int ny = cy + (ty - cy) * j / 3 + random(-15, 15);
      if (j == 3) {
        nx = tx;
        ny = ty;
      }
      tft.drawLine(lx, ly, nx, ny, color);
      lx = nx;
      ly = ny;
    }
  }

  // Draw Stylized Text
  tft.setTextSize(3);
  tft.setTextDatum(MC_DATUM);

  // Outer Glow
  tft.setTextColor(TFT_BLUE);
  tft.drawString("ELECTROHARP", cx + 1, cy + 1);
  tft.setTextColor(TFT_NAVY);
  tft.drawString("ELECTROHARP", cx - 1, cy - 1);

  // Main Text
  tft.setTextColor(TFT_WHITE);
  tft.drawString("ELECTROHARP", cx, cy);
}

// --- BOOT MENU UI ---
void drawAudioConfigScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Audio Config", SCREEN_WIDTH / 2, 20);

  // Status
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Current: ", 20, 60);
  tft.setTextColor(TFT_CYAN);

  if (settings.audioProfileIndex >= 0 &&
      settings.audioProfileIndex < AUDIO_PRESET_COUNT) {
    tft.drawString(audioPresets[settings.audioProfileIndex].name, 80, 60);
  } else {
    tft.drawString("Unknown", 80, 60);
  }

  // Instructions
  tft.setTextColor(TFT_LIGHTGREY);
  tft.drawString("Cycle with 'Audio Config' Btn", 20, 90);
  tft.drawString("Test with 'Cycle Test' Btn", 20, 110);

  // Test Status
  if (isAudioTestRunning) {
    tft.setTextColor(TFT_GREEN);
    tft.drawString("TESTING AUDIO...", 20, 140);
  } else {
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("Test Initialized", 20, 140);
  }

  // Draw Buttons
  int btnY = 180;
  int btnH = 40;
  int btnW = 100;

  // Cycle Preset Btn
  tft.fillRect(20, btnY, btnW, btnH, TFT_BLUE);
  tft.drawRect(20, btnY, btnW, btnH, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Cycle", 20 + btnW / 2, btnY + btnH / 2);

  // Test Btn
  tft.fillRect(140, btnY, btnW, btnH, TFT_RED);
  tft.drawRect(140, btnY, btnW, btnH, TFT_WHITE);
  tft.drawString("Test Tone", 140 + btnW / 2, btnY + btnH / 2);

  // Back Btn
  tft.fillRect(260, btnY, 40, btnH, TFT_DARKGREY);
  tft.drawRect(260, btnY, 40, btnH, TFT_WHITE);
  tft.drawString("OK", 260 + 20, btnY + btnH / 2);
}

void updateButtonVisuals() {

  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  // 8 Buttons at BOTTOM
  int btnCount = 8;
  int w = SCREEN_WIDTH / btnCount;
  int h = 50;                // Taller by 10px
  int y = SCREEN_HEIGHT - h; // Bottom (Starts at 190)

  // Bank Colors: Green, Cyan, Yellow, Magenta, Orange, Grey (Off)
  // User wants Octave to match Editor Sliders (CYAN).
  // Colors for Chords can remain.
  uint16_t bankColors[] = {TFT_GREEN,   TFT_CYAN,   TFT_YELLOW,
                           TFT_MAGENTA, TFT_ORANGE, TFT_LIGHTGREY};

  for (int i = 0; i < btnCount; i++) {
    int x = i * w;
    uint16_t color = TFT_BLACK; // Default Inactive Background
    const char *label = "";
    int textColor = TFT_WHITE; // Default Text

    if (i == 0) { // Octave Down
      label = "<";
      if (octaveShift < 0)
        // Active: Mix Black -> Cyan based on intensity?
        // Or just Cyan? Existing logid used alphaBlend.
        // Let's blend from Black to Cyan.
        color = alphaBlend(TFT_BLACK, TFT_CYAN, abs(octaveShift) / 3.0f);
    } else if (i == 7) { // Octave Up
      label = ">";
      if (octaveShift > 0)
        color = alphaBlend(TFT_BLACK, TFT_CYAN, abs(octaveShift) / 3.0f);
    } else {
      // Chord Buttons (1-6)
      if (activeButtonIndex == -1) {   // Chromatic mode
        label = chordLabels[i - 1][0]; // Show Bank 0 Label (Default)
        color = TFT_BLACK;             // Default Inactive
      } else if (activeButtonIndex == i) {
        label = chordLabels[i - 1][buttonStates[i]];
        color = bankColors[buttonStates[i]];
        textColor = TFT_BLACK;
      } else {
        // Other buttons while one is active
        label = "Off";
        color = TFT_BLACK;
      }
    }

    tft.fillRect(x, y, w, h, color);
    tft.drawRect(x, y, w, h, TFT_WHITE); // Always White Outline
    tft.setTextColor(textColor);
    tft.drawString(label, x + (w / 2), y + (h / 2));
  }
}

void drawInterface() {
  drawStrings();
  drawArpButton(); // Add Arp
  drawWaveButton();
  drawDelayButton();
  drawFXButtons();
  drawVolumeSlider();
  drawTransposeButton(); // Add Transpose
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
  updateActiveNotes();   // Update Arp List
  delay(150);            // Debounce
}

// --- SETUP FUNCTIONS ---
// --- SETUP FUNCTIONS ---
// --- Setup Functions ---
void setupSpeaker() {
  Serial.println("Initializing Speaker (Internal DAC)...");

  // CRITICAL: Stop Bluetooth safely to prevent crash if not started
  if (isBluetoothActive) {
    Serial.println("Stopping Bluetooth...");
    a2dp_source.end();
    isBluetoothActive = false;
  }

  // Set Profile
  currentProfile = &spkProfile;
  Serial.println("Sound Profile: SPEAKER (High Output)");

  // FORCE 22050Hz for Speaker Stability
  activeSampleRate = 22050;

  // SAFETY: Ensure Timer corresponds to sample rate, but DO NOT re-attach ISR
  // Re-attaching an active ISR can cause crashes or detachments.
  if (timer != NULL) {
    timerAlarmWrite(timer, 1000000 / activeSampleRate, true);
    // timerAlarmEnable(timer); // Already enabled in setup
  }

  // CRITICAL: Re-Enable DAC and Amplifier (Recover from BT Mode)
  dac_output_enable(DAC_CHANNEL_2); // GPIO 26

  // Enable Amplifier (FM8002E Shutdown is Pin 4 - Active High Shutdown -> LOW =
  // Enabled)
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);

  // Ensure Power Rails are Up
  int shotgunHigh[] = {21, 27, 22};
  for (int p : shotgunHigh) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }

  updateDerivedParameters(); // Ensure LFOs match rate

  // Clear and Reset Pointer
  bufReadHead = 0;
  bufWriteHead = 0;
  for (int i = 0; i < AUDIO_BUF_SIZE; i++)
    audioBuffer[i] = 128;
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
    // Serial.printf("Discovered: %s (%d)\n", ssid, rssi); // DISABLE SPAM
  }

  return false; // Keep scanning
}

void setupBluetooth(bool scanning = false) {
  Serial.println("Initializing Bluetooth...");
  currentProfile = &btProfile;
  Serial.println("Sound Profile: BLUETOOTH (High Fidelity)");

  // FORCE 44100Hz for Bluetooth
  activeSampleRate = 44100;
  // CRITICAL: DISABLE Speaker Timer Interrupt!
  // The speaker ISR consumes CPU and conflicts with BT stack.
  if (timer != NULL) {
    timerAlarmDisable(timer);
    timerDetachInterrupt(timer);
  }
  updateDerivedParameters();

  a2dp_source.set_reset_ble(true);
  a2dp_source.set_on_connection_state_changed(connection_state_changed);
  a2dp_source.set_ssid_callback(ssid_callback);
  a2dp_source.set_data_callback_in_frames(bt_data_stream_callback);

  if (scanning) {
    a2dp_source.set_auto_reconnect(false);
  } else {
    a2dp_source.set_auto_reconnect(true);
  }

  Serial.println("Starting Bluetooth...");
  a2dp_source.start();
  isBluetoothActive = true;
  a2dp_source.set_volume(100);

  delay(1000);

  Serial.println("Re-claiming SPI/Touch pins...");
  SPI.end();
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  SPI.setFrequency(27000000);
  ts.begin();
  ts.setRotation(1);

  Serial.println("Bluetooth Scanning Started...");
}

// --- Middle C Centering Helper ---
int getVisualStringOffset() {
  int numStrings = activeParams.octaveRange * 12 + 1;
  int visibleCenter = numStrings / 2;
  int targetCenterIndex =
      24; // C4 (Middle C) is index 24 in baseFreqs (Starts C2)
  // We want visual index 'visibleCenter' to map to 'targetCenterIndex'
  // i + offset = target -> offset = target - i
  int offset = targetCenterIndex - visibleCenter;
  // Clamp boundaries to avoid OOB
  if (offset < 0)
    offset = 0;
  if (offset + numStrings > STRING_COUNT)
    offset = STRING_COUNT - numStrings;
  return offset;
}

int getGlobalNoteIndex(int sIdx) {
  return sIdx + getVisualStringOffset() + (octaveShift * 12);
}

// Internal version for safe array access
int getGlobalNoteIndexSafe(int sIdx) {
  int idx = getGlobalNoteIndex(sIdx);
  return constrain(idx, 0, STRING_COUNT - 1);
}

// --- APP MODES ---
enum AppMode { MODE_PLAY, MODE_EDIT };
AppMode currentMode = MODE_PLAY;
bool editorInputBlocked = false; // Prevent bounce on entry
// uint32_t wavePressStart = 0; // Moved to Top

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
      if (val == 7) // TARGET_LFO_DEPTH
        s = "LFOD";
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
    // Non-Linear Display for LFO Hz (Index 0)
    // If we passed "LFO Hz", we need to invert the quad/cubic map.
    // norm = sqrt((val - min)/(max-min)).
    float norm = 0.0f;
    if (String(label) == "LFO Hz") {
      // Range 0.08 - 16.0
      float ratio = (val - min) / (max - min);
      if (ratio < 0)
        ratio = 0;

      // Quadratic: val = min + norm^2 * (max-min)
      // norm^2 = ratio -> norm = sqrt(ratio)
      norm = sqrt(ratio);
    } else {
      norm = (val - min) / (max - min);
    }

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
                    (float)activeParams.lfoTarget, 0, 8, true);
  drawSliderControl(2 * topW, topY, topW, topH, "LFO Type",
                    (float)activeParams.lfoType, 0, 3, true);
  drawSliderControl(3 * topW, topY, topW, topH, "Range",
                    (float)activeParams.octaveRange, 1, 8, true);

  // Bottom Rows (1,2,3): 3 Cols
  int botW = SCREEN_WIDTH / 3;
  int botH = SCREEN_HEIGHT / 4;

  // Row 1 (y = topH) -> LFO Hz, LFO Depth, Drive
  int r1 = topH;
  drawSliderControl(0 * botW, r1, botW, botH, "LFO Hz", activeParams.lfoRate,
                    0.08, 16.0);
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

      // Visual Feedback (Yellow->Red Gradient)
      uint16_t c = getGradientColor(editorPianoPressStart, 250);
      tft.drawRect(rx, ry, rw, rh, c);

      // Hold > 250ms -> Switch Wave
      if (!editorPianoHandled && millis() - editorPianoPressStart > 250) {
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
      if (v > 7) // Cycle 0..7 (TARGET_LFO_DEPTH is 7)
        v = 0;
      activeParams.lfoTarget = (LfoTarget)v;
      drawSliderControl(rx, ry, rw, rh, "LFO Tgt", (float)v, 0, 7, true);
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
    case 3: // Octave Range
    {
      int v = activeParams.octaveRange;
      v++;
      if (v > 8)
        v = 1;
      activeParams.octaveRange = v;
      drawSliderControl(rx, ry, rw, rh, "Range", (float)v, 1, 8, true);
      lastTopPress = millis();
      // REMOVED: tft.fillScreen(COLOR_BG); -- Avoid black screen on toggle
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
      minV = 0.08; // Expanded Range (was 0.1)
      maxV = 16.0; // Expanded Range (was 10.0)
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
      label = "Resonance";
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
      label = "Tremolo Hz";
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
    if (sliderTarget == &activeParams.lfoRate) {
      // Non-Linear (Square) for LFO Hz
      // val = min + (norm * norm) * (max - min)
      *sliderTarget = minV + (norm * norm) * (maxV - minV);
    } else {
      *sliderTarget = minV + norm * (maxV - minV);
    }

    // Redraw
    drawSliderControl(rx, ry, rw, rh, label, *sliderTarget, minV, maxV, false);
    updateDerivedParameters();
  }
}

// --- BOOT UI ---
void drawBootScreen() {
  tft.fillScreen(TFT_BLACK);

  // Alternating Title "Electroharp"
  tft.setTextSize(2);
  const char *title = "Electroharp";
  int totalWidth = tft.textWidth(title);
  int startX = (SCREEN_WIDTH - totalWidth) / 2;
  int titleY = 5; // Moved up 5px

  tft.setCursor(startX, titleY);
  for (int i = 0; i < strlen(title); i++) {
    // Alternate Blue (0x9E7D) and Yellow (0xFFF0)
    uint16_t color = (i % 2 == 0) ? 0x9E7D : 0xFFF0;
    tft.setTextColor(color);
    tft.print(title[i]);
  }

  // Version - draw centered below the whole title
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("v1.2", SCREEN_WIDTH / 2,
                 titleY + 18); // Slightly tighter to title

  tft.setTextDatum(MC_DATUM);

  // Split Screen
  // Left: Bluetooth
  tft.fillRect(0, 40, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 100, TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.drawString("BLUETOOTH", SCREEN_WIDTH / 4, 40 + (SCREEN_HEIGHT - 100) / 2);

  // Right: Speaker
  tft.fillRect(SCREEN_WIDTH / 2, 40, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 100,
               TFT_DARKGREEN);
  tft.drawString("SPEAKER", 3 * SCREEN_WIDTH / 4,
                 40 + (SCREEN_HEIGHT - 100) / 2);

  // Bottom Center: Config
  tft.fillRect(SCREEN_WIDTH / 2 - 60, SCREEN_HEIGHT - 50, 120, 40, TFT_MAGENTA);
  tft.drawRect(SCREEN_WIDTH / 2 - 60, SCREEN_HEIGHT - 50, 120, 40, TFT_WHITE);
  tft.drawString("CONFIG", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 30);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Select Audio Output", SCREEN_WIDTH / 2,
                 30); // Moved up 5px from 35
  tft.setTextColor(TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Hold 3s to Calibrate", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 5);
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

void setupLEDs() {}
Preferences preferences;

// Forward Declaration
void saveSettings();

void loadSettings() {
  settings.begin(); // Initialize Preferences
  settings.load();
  // Apply loaded settings immediately where relevant
  if (settings.audioProfileIndex >= 0 &&
      settings.audioProfileIndex < AUDIO_PRESET_COUNT) {
    // Map index to profile structure if needed?
    // Actually our audioPresets struct just has metadata.
    // The ACTUAL profile tuning (gainSaw, etc.) is in 'btProfile' /
    // 'spkProfile' This part of the request was "persist audio settings". If we
    // want to persist which *device* was selected, that's
    // Settings::audioProfileIndex. The code currently doesn't switch profiles
    // based on this index unless index 0=Speaker, 1=BT? Let's assume for now we
    // just load the index. The User selects Audio Output in Boot Menu anyway.
  }
}

void saveSettings() { settings.save(); }

void setup() {
  Serial.begin(115200);

  // INCREASE WDT to 30s to allow Bluetooth Handshake
  esp_task_wdt_init(30, true);

  delay(500);
  Serial.println("\n--- ELECTROHARP 2.8 BOOT ---");

  // FM8002E Shutdown (GPIO 4) is Active High Shutdown -> LOW = Enabled
  Serial.println("Enabling Amp (GPIO 4)...");
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);

  // Potential Power/Enable Pins
  int shotgunHigh[] = {21, 27, 22};
  for (int p : shotgunHigh) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }

  // --- DAC HARDWARE INIT ---
  Serial.println("Initializing DAC...");
  dac_output_enable(DAC_CHANNEL_2);
  dacWrite(26, 128);

  // --- STARTUP TONE TEST ---
  Serial.println("Running Audio Test Tone...");
  for (int i = 0; i < 88; i++) {
    dacWrite(26, 200);
    delayMicroseconds(1136);
    dacWrite(26, 50);
    delayMicroseconds(1136);
  }
  dacWrite(26, 128);

  Serial.println("Loading Settings...");
  loadSettings();

  Serial.println("Initializing Voices...");
  SynthVoice::initLUT();

  // Initialize Wave Presets
  for (int i = 0; i < 4; i++) {
    wavePresets[i] = activeParams;
  }

  // Init Delay Buffer - Statically allocated now
  Serial.printf("Delay Buffer Size: %d bytes\n", MAX_DELAY_LEN * 2);

  Serial.println("Initializing Display...");
  tft.init();
  tft.setRotation(1);

  // High-Fidelity Splash Animation (3x 300ms)
  for (int i = 0; i < 3; i++) {
    drawSplashScreen();
    delay(300);
  }

  tft.fillScreen(TFT_BLACK);

  // Init Touch (using global SPI, re-mapped)
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin();
  ts.setRotation(1);

  // Power-Cycle / Flush Touch Controller
  for (int i = 0; i < 5; i++)
    ts.getPoint();

  // Init Audio Frequencies
  float k = 1.059463094359;
  float f = 65.406f; // C2 (Starting Point)
  for (int i = 0; i < STRING_COUNT; i++) {
    baseFreqs[i] = f;
    f *= k;
  }

  // --- AUDIO TIMER SETUP (DAC) ---
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(timer);

  // Enable DAC Output
  dac_output_enable(DAC_CHANNEL_2); // GPIO 26

  // Show Boot Selection or Force Calibration
  delay(500);

  if (!settings.touch.isCalibrated) {
    audioTarget = TARGET_CALIBRATION_1;
    drawCalibrationScreen(1);
  } else {
    audioTarget = TARGET_BOOT; // Set to boot mode
    drawBootScreen();          // Draw boot screen initially
  }
}
// --- HELPER FUNCTION: Find String Visual ID ---
int getClosestStringIndex(float targetFreq) {
  // Fix: If Frequency is below base range (Sparkle 2 Down mode), wrap
  while (targetFreq < baseFreqs[0] && targetFreq > 1.0f) {
    targetFreq *= 2.0f;
  }
  while (targetFreq > baseFreqs[STRING_COUNT - 1] * 2.0f) {
    targetFreq *= 0.5f;
  }

  float minDiff = 100000.0f;
  int bestIdx = 0;
  // Brute force (Fast enough for 37 strings)
  for (int i = 0; i < STRING_COUNT; i++) {
    float diff = fabs(baseFreqs[i] - targetFreq);
    if (diff < minDiff) {
      minDiff = diff;
      bestIdx = i;
    }
  }
  return bestIdx;
}

// --- HELPER FUNCTION: Trigger Note ---
void triggerNote(int sIdx) {
  int gIdx = getGlobalNoteIndexSafe(sIdx);
  float freq = baseFreqs[gIdx];

  // Anti-Aliasing Cap for 22kHz (Lowered to 4.8kHz for stable BT/Speaker)
  if (freq > 4800.0f)
    freq = 4800.0f;
  // Logic: Check Chord Mask & Inversion Cutoff
  int btnState = (activeButtonIndex >= 1 && activeButtonIndex <= 6)
                     ? buttonStates[activeButtonIndex]
                     : 0;
  int invCutoff = 0;
  if (btnState >= 2 && btnState <= 4) {
    invCutoff = getInversionCutoff(currentChordMask, btnState - 1);
  }

  // Check Mask
  if (!((currentChordMask & (1 << (gIdx % 12))) && (gIdx >= invCutoff))) {
    return; // Note not in chord, ignore
  }

  // --- GOVERNOR: Update Strum History ---
  noteTriggerTimes[noteTriggerHead] = millis();
  noteTriggerHead = (noteTriggerHead + 1) % 5;

  // Sine pitch normalization (User request: same default as other waves)
  int extraOctave = 0;
  /* Previously Sine had +2 octaves for speaker:
  if (currentProfile == &spkProfile && currentWaveform == WAVE_SINE) {
    extraOctave = 2;
  }
  */

  // Apply Global Octave Shift + Extra Boost
  int totalTwist = octaveShift + extraOctave;
  if (totalTwist > 0) {
    for (int k = 0; k < totalTwist; k++)
      freq *= 2.0f;
  } else if (totalTwist < 0) {
    for (int k = 0; k < abs(totalTwist); k++)
      freq *= 0.5f;
  }

  // Apply Transpose (Root Note Shift)
  freq *= pow(2.0f, rootNote / 12.0f);

  // Find Voice
  int vIdx = -1;
  int activeCount = 0;

  // 1. Count Active Voices
  for (int v = 0; v < MAX_VOICES; v++) {
    if (voices[v].active)
      activeCount++;
  }

  // 2. Try to find Empty Slot (IF and ONLY IF we are under the Governor Limit)
  if (activeCount < maxPolyphony) {
    for (int v = 0; v < MAX_VOICES; v++) {
      if (!voices[v].active) {
        vIdx = v;
        break;
      }
    }
  }

  // 3. Stealing Logic (If capped OR no empty slots)
  if (vIdx == -1) {
    // Priority A: Steal Released (Not Held) & Unlatched
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active && !voices[v].held && !voices[v].isLatchedArp) {
        vIdx = v;
        break;
      }
    }
  }

  if (vIdx == -1) {
    // Priority B: Steal Any Unlatched (Oldest/First Found)
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active && !voices[v].isLatchedArp) {
        vIdx = v;
        break;
      }
    }
  }

  // Priority C: Desperation (Steal 0) if everything is latched?
  if (vIdx == -1)
    vIdx = 0;

  // Trigger Note
  // Trigger Note
  voices[vIdx].trigger(freq, sIdx);
  voices[vIdx].isSparkle = false; // Main Strum follows Global ADSR
  voices[vIdx].isLatchedArp =
      false; // Ensure unlatched voices are marked correctly
  stringEnergy[sIdx] = 1.0f;

  // --- SPARKLE MODE LOGIC ---
  if (arpMode == ARP_SPARKLE) {
    noteTriggerCounter++;
    int sparkIdx;
    if (noteTriggerCounter % 5 == 0) {
      float f = freq;
      float ratio = 8.0f / 9.0f;
      // Repeat 1
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(350, f, sparkIdx, 0.15f, 0.75f);
      // Repeat 2
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(700, f, sparkIdx, 0.15f, 0.25f);
    } else if (noteTriggerCounter % 4 == 0) {
      float f = freq;
      float ratio = 10.0f / 9.0f;
      // Spark 1
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(150, f, sparkIdx, 0.10f, 1.0f);
      // Spark 2
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(300, f, sparkIdx, 0.10f, 0.75f);
      // Spark 3
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(450, f, sparkIdx, 0.10f, 0.50f);
      // Spark 4
      f = f * ratio;
      sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
      scheduleSpark(600, f, sparkIdx, 0.10f, 0.25f);
    }
  } else if (arpMode == ARP_SPARKLE2) {
    noteTriggerCounter++;

    if (noteTriggerCounter % 7 == 0) {
      float ratio = 0.5612f;
      float f = freq;
      float vels[] = {0.75f, 0.50f, 0.25f};
      for (int k = 1; k <= 3; k++) {
        f *= ratio;
        int sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
        scheduleSpark(650 * k, f, sparkIdx, 0.15f, vels[k - 1]);
      }
    } else if (noteTriggerCounter % 5 == 0) {
      float ratio = 1.4983f;
      float f = freq;
      float vels[] = {0.75f, 0.50f, 0.25f, 0.10f};
      for (int k = 1; k <= 4; k++) {
        f *= ratio;
        int sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
        scheduleSpark(350 * k, f, sparkIdx, 0.15f, vels[k - 1]);
      }
    } else if (noteTriggerCounter % 3 == 0) {
      float ratio = 1.2599f;
      float f = freq;
      float vels[] = {1.00f, 0.85f, 0.70f, 0.55f, 0.40f, 0.25f, 0.10f};
      for (int k = 1; k <= 7; k++) {
        f *= ratio;
        int sparkIdx = getClosestStringIndex(baseFreqs[sIdx] * (f / freq));
        scheduleSpark(150 * k, f, sparkIdx, 0.10f, vels[k - 1]);
      }
    }
  }
}

void fireArp(int octaveOffset, bool latched) {
  if (activeNoteCount == 0)
    return;

  // Safety: If Latched, release PREVIOUS Latched voice
  if (latched) {
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active && voices[v].isLatchedArp) {
        voices[v].release();
        // We don't break, in case multiple got stuck
      }
    }
  }

  int noteIdx = 0; // Index into activeNotes

  // Determine Note based on Mode
  switch (arpMode) {
  case ARP_UP:
    noteIdx = arpStep % activeNoteCount;
    arpStep++; // Linear wrap handled by modulus next time or reset
    if (arpStep >= activeNoteCount)
      arpStep = 0;
    break;
  case ARP_DOWN:
    noteIdx = (activeNoteCount - 1) - (arpStep % activeNoteCount);
    arpStep++;
    if (arpStep >= activeNoteCount)
      arpStep = 0;
    break;
  case ARP_UPDOWN:
    // 0, 1, 2, 1, 0... Length = 2*N - 2
    if (activeNoteCount > 1) {
      int cycleLen = (activeNoteCount * 2) - 2;
      int pos = arpStep % cycleLen;
      if (pos < activeNoteCount) {
        noteIdx = pos;
      } else {
        noteIdx = activeNoteCount - 2 - (pos - activeNoteCount);
      }
      arpStep++;
      if (arpStep >= cycleLen)
        arpStep = 0;
    } else {
      noteIdx = 0;
      arpStep = 0;
    }
    break;
  case ARP_WALK:
    // "Staggered Walk": 1, 3, 2, 4, 3, 5... (0, 2, 1, 3, 2, 4...)
    // Logic: Even steps = i, Odd steps = i+2?
    // Pattern: +2, -1, +2, -1...
    // Let's implement a simple algorithmic walk.
    // Or pre-calc? Algorithmic is funnier.
    // Step 0: 0. Step 1: 2. Step 2: 1. Step 3: 3.
    // Basically: If even, idx = step/2. If odd, idx = (step/2) + 2.
    // Need to wrap carefully.
    {
      int base = (arpStep / 2) % activeNoteCount;
      if (arpStep % 2 == 0) {
        noteIdx = base;
      } else {
        noteIdx = (base + 2) % activeNoteCount;
      }
      arpStep++;
      // Wrap logic is tricky for infinite walk. Let's just ++.
      if (arpStep >= activeNoteCount * 2)
        arpStep = 0;
    }
    break;
  case ARP_RANDOM:
    noteIdx = randomSequence[arpStep % activeNoteCount];
    arpStep++;
  default:
    break;
  }

  // Play the Note
  // Add Octave Offset from Strum Position
  int stringIndex = activeNotes[noteIdx] + octaveOffset;

  // Bounds Check
  if (stringIndex >= STRING_COUNT) {
    return; // Don't play out of bounds
  }

  // Trigger Logic for Latch vs Unlatched
  if (latched) {
    // Find Voice
    int vIdx = -1;
    // 1. Find Inactive
    for (int i = 0; i < MAX_VOICES; i++) {
      if (!voices[i].active) {
        vIdx = i;
        break;
      }
    }
    // 2. Steal Oldest (if not found)
    if (vIdx == -1)
      vIdx = 0; // Naive steal

    // Freq Calculation (Duplicated from triggerNote slightly, but access
    // needed)
    float freq = baseFreqs[stringIndex];
    // Apply Global Octave Shift
    int effectiveOctave = latched ? latchedOctaveShift : octaveShift;
    if (effectiveOctave != 0) {
      freq *= pow(2.0f, effectiveOctave);
    }
    // Apply Transpose
    freq *= pow(2.0f, rootNote / 12.0f);

    // Trigger logic
    // Custom Envelope: Attack (Active), Decay (0.5), Sustain (0.6), Release
    // (2.5s)
    float sus = 0.6f;
    float rel = 2.5f;
    if (arpMode == ARP_SPARKLE || arpMode == ARP_SPARKLE2) {
      sus = 0.0f;
      rel = 0.15f;
    }
    voices[vIdx].trigger(freq, stringIndex, currentWaveform, globalPulseWidth,
                         activeParams.attackTime, 0.5f, sus, rel);
    voices[vIdx].isLatchedArp = true;
  } else {
    // Enforce Monophony for Manual Arp (Release other unlatched voices)
    // This cleans up the "chordal" confusion when strumming fast
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active && !voices[v].isLatchedArp) {
        voices[v].release();
      }
    }

    triggerNote(stringIndex);
    // Note: triggerNote does NOT set isLatchedArp=false explicitely,
    // but default constructor or previous clear handles it.
    // However, since we just reused a voice, let's be safe.
    // But we don't know which voice triggerNote picked!
    // We updated triggerNote earlier to set `isLatchedArp = false`.
  }
}

void loop() {
  static bool waitForArpRelease = false;
  static uint32_t lastArpClock = 0; // Fix Missing Static in Loop

  // Robust Clear: Ensure flag resets if screen is not touched, regardless of
  // Mode or Return path
  if (!ts.touched()) {
    waitForArpRelease = false;
  }
  // --- SPARKLE PROCESSING ---
  if (arpMode == ARP_SPARKLE || arpMode == ARP_SPARKLE2) {
    uint32_t now = millis();
    for (int i = 0; i < MAX_SPARKS; i++) {
      if (pendingSparks[i].active && now >= pendingSparks[i].triggerTime) {
        // Compute active voice idx? Or just fire on next available?
        // Reuse logic from 'triggerNote' or similar.
        // We need to find an available voice or steal one.
        // Simplified: Loop active voices or just pick one?
        // Standard Trigger Logic:
        int vIdx = -1;
        // 1. Find Free
        for (int v = 0; v < MAX_VOICES; v++) {
          if (!voices[v].active) {
            vIdx = v;
            break;
          }
        }
        // 2. Steal Oldest (if none free)
        if (vIdx == -1) {
          // Simple steal 0 for now or improved logic?
          vIdx = 0;
        }

        voices[vIdx].trigger(pendingSparks[i].freq, pendingSparks[i].stringIdx,
                             currentWaveform, globalPulseWidth, 0.00f,
                             pendingSparks[i].releaseTime, 0.0f,
                             pendingSparks[i].releaseTime);
        voices[vIdx].isSparkle = true;
        stringEnergy[pendingSparks[i].stringIdx] = 1.0f;

        pendingSparks[i].active = false;
      }
    }
  }

  // --- ARP LATCH TIMER (Global) ---
  if (arpMode != ARP_OFF && arpLatch) {
    float rate = activeParams.lfoRate;
    if (rate < 0.1f)
      rate = 0.1f;
    uint32_t interval = 1000.0f / (rate * 0.20f); // 20% Speed for "Drone"

    if (millis() - lastArpClock > interval) {
      fireArp(0, true); // Latched Call
      lastArpClock = millis();
    }
  }

  // --- BOOT MENU & CALIBRATION ---
  if (audioTarget == TARGET_BOOT || audioTarget == TARGET_CALIBRATION_1 ||
      audioTarget == TARGET_CALIBRATION_2 ||
      audioTarget == TARGET_PANIC_CONFIRM) {

    // Calibration Logic
    static uint32_t lastCalStepTime = 0;

    if (audioTarget == TARGET_PANIC_CONFIRM) {
      if (ts.touched()) {
        TS_Point p = ts.getPoint();
        if (p.z > 200) {
          // Tap to Dismiss & Start Calibration
          audioTarget = TARGET_CALIBRATION_1;
          drawCalibrationScreen(1);
          lastCalStepTime = millis();
          delay(500);
        }
      }
      return;
    }

    if (audioTarget == TARGET_CALIBRATION_1) {
      if (millis() - lastCalStepTime > 500 && ts.touched()) {
        TS_Point p = ts.getPoint();
        if (p.z > 500) { // Firm press
          // Store Top-Left Raw
          settings.touch.minX = p.x;
          settings.touch.minY = p.y;

          audioTarget = TARGET_CALIBRATION_2;
          drawCalibrationScreen(2);
          lastCalStepTime = millis();
          delay(500); // UI Debounce
        }
      }
      return;
    }

    if (audioTarget == TARGET_CALIBRATION_2) {
      if (millis() - lastCalStepTime > 500 && ts.touched()) {
        TS_Point p = ts.getPoint();
        if (p.z > 500) {
          // Store Bottom-Right Raw
          settings.touch.maxX = p.x;
          settings.touch.maxY = p.y;
          settings.touch.isCalibrated = true;

          // Normalize (detect if min > max)
          if (settings.touch.minX > settings.touch.maxX) {
            uint16_t temp = settings.touch.minX;
            settings.touch.minX = settings.touch.maxX;
            settings.touch.maxX = temp;
          }
          if (settings.touch.minY > settings.touch.maxY) {
            uint16_t temp = settings.touch.minY;
            settings.touch.minY = settings.touch.maxY;
            settings.touch.maxY = temp;
          }

          drawCalibrationScreen(3);
          settings.save();
          delay(1000);

          audioTarget = TARGET_BOOT;
          drawBootScreen();
          lastCalStepTime = 0;
        }
      }
      return;
    }

    // Boot Menu Logic
    static uint32_t bootModeStart = 0;
    static AudioTarget lastTarget =
        TARGET_SPEAKER; // Force initial mismatch check logic if needed

    // Simple state check: if we just entered this block?
    // Actually, we can just check a flag or similar.
    // Or just trust the user flow.
    // Let's ensure drawBootScreen IS called if we came from Calibration.
    // In Calibration logic, we call drawBootScreen() before setting
    // TARGET_BOOT. So it should be fine.
    if (bootModeStart == 0) {
      bootModeStart = millis();
    }

    static TS_Point lastBootTouch;
    static uint32_t bootTouchStartLocal = 0;
    static bool isBootTouching = false;
    static bool panicWaitingForRelease = false;

    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      if (p.z > 200) {
        lastBootTouch = p;
        if (!isBootTouching) {
          isBootTouching = true;
          bootTouchStartLocal = millis();
        } else {
          // Check Long Press (Panic)
          if (!panicWaitingForRelease &&
              (millis() - bootTouchStartLocal > 3000)) {
            // Trigger Panic Visuals
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE);
            tft.drawString("PANIC TRIGGERED", SCREEN_WIDTH / 2,
                           SCREEN_HEIGHT / 2);
            tft.drawString("Release to Calibrate", SCREEN_WIDTH / 2,
                           SCREEN_HEIGHT / 2 + 30);
            panicWaitingForRelease = true;
          }
        }
      }
    } else {
      // Released?
      if (isBootTouching) {
        isBootTouching = false;

        if (panicWaitingForRelease) {
          panicWaitingForRelease = false;
          audioTarget = TARGET_PANIC_CONFIRM;
          tft.fillScreen(TFT_RED);
          tft.setTextColor(TFT_WHITE);
          tft.setTextSize(2);
          tft.setTextDatum(MC_DATUM);
          tft.drawString("PANIC TRIGGERED", SCREEN_WIDTH / 2,
                         SCREEN_HEIGHT / 2 - 20);
          tft.setTextSize(1);
          tft.drawString("READY TO CALIBRATE", SCREEN_WIDTH / 2,
                         SCREEN_HEIGHT / 2 + 10);
          tft.drawString("Tap Screen to Start", SCREEN_WIDTH / 2,
                         SCREEN_HEIGHT / 2 + 40);
          return;
        }

        // ACTION ON RELEASE (if not long hold)
        // Use raw coordinates or default calibration
        int touchX = map(lastBootTouch.x, settings.touch.minX,
                         settings.touch.maxX, 0, SCREEN_WIDTH);
        int touchY = map(lastBootTouch.y, settings.touch.minY,
                         settings.touch.maxY, 0, SCREEN_HEIGHT);

        // Clamp
        if (touchX < 0)
          touchX = 0;
        if (touchX >= SCREEN_WIDTH)
          touchX = SCREEN_WIDTH - 1;
        if (touchY < 0)
          touchY = 0;
        if (touchY >= SCREEN_HEIGHT)
          touchY = SCREEN_HEIGHT - 1;

        if (touchY > SCREEN_HEIGHT - 60) {
          // Config Area
          if (touchX > SCREEN_WIDTH / 2 - 70 &&
              touchX < SCREEN_WIDTH / 2 + 70) {
            audioTarget = TARGET_CONFIG;
            drawConfigMenu();
            delay(250);
            return;
          }
        } else if (touchY > 40) { // Main Area
          if (touchX < SCREEN_WIDTH / 2) {
            // Bluetooth
            audioTarget = TARGET_BT_SELECT;
            foundDeviceCount = 0;
            setupBluetooth(true);
            drawBTSelectScreen(-1);
            return;
          } else {
            // Speaker
            audioTarget = TARGET_SPEAKER;
            setupSpeaker();
            tft.fillScreen(TFT_BLACK);
            drawInterface();
            return;
          }
        }
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
          if (isBluetoothActive) {
            a2dp_source.end(); // Stop
            isBluetoothActive = false;
          }
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
            activeSampleRate = 44100; // Switch to BT rate
            updateDerivedParameters();

            // CRITICAL: Disable DAC Timer so it doesn't fight BT
            if (timer != NULL) {
              timerAlarmDisable(timer);
              // timerDetachInterrupt(timer); // Removed to prevent crash/reboot
            }
            dac_output_disable(DAC_CHANNEL_2); // Free the pin

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
  // --- PERFORMANCE MODE ---
  if (audioTarget == TARGET_SPEAKER || audioTarget == TARGET_BLUETOOTH ||
      audioTarget == TARGET_CONFIG || audioTarget == TARGET_AUDIO_CONFIG) {

    if (audioTarget == TARGET_SPEAKER) {
      fillAudioBuffer();
    }

    // Refresh Visuals
    static uint32_t lastVis = 0;
    if (millis() - lastVis > 15) {
      if (currentMode == MODE_PLAY &&
          (audioTarget == TARGET_SPEAKER || audioTarget == TARGET_BLUETOOTH)) {
        updateStringVisuals();
      }
      lastVis = millis();
    }

    // Editor Mode
    if (currentMode == MODE_EDIT) {
      static uint32_t releaseDebounceStart = 0;
      if (!ts.touched()) {
        if (releaseDebounceStart == 0)
          releaseDebounceStart = millis();
        if (millis() - releaseDebounceStart > 50) {
          if (editorPianoPressStart > 0) {
            if (!editorPianoHandled && millis() - editorPianoPressStart < 700) {
              currentMode = MODE_PLAY;
              tft.fillScreen(COLOR_BG);
              drawInterface();
              delay(200);
              inputBlockTimer = millis() + 500;
            }
            editorPianoPressStart = 0;
          }
          editorInputBlocked = false;
        }
      } else {
        releaseDebounceStart = 0;
        if (!editorInputBlocked) {
          TS_Point p = ts.getPoint();
          if (p.z > 400) {
            int tx = constrain(map(p.x, 380, 3700, 0, 320), 0, 320);
            int ty = constrain(map(p.y, 450, 3700, 0, 240), 0, 240);
            handleEditorTouch(tx, ty);
          }
        }
      }
      return;
    }

    // Periodically Sync Parameters (~50Hz)
    static uint32_t lastParamSync = 0;
    if (millis() - lastParamSync > 20) {
      // Since LDR is removed, we just ensure globals match activeParams
      // Filter frequency and resonance are primarily updated here for the
      // UI/Editor
      svf_f = 2.0f * sin(PI * activeParams.filterCutoff / (float)SAMPLE_RATE);
      if (svf_f > 0.95f)
        svf_f = 0.95f;
      svf_q = 1.0f - activeParams.filterRes;

      globalPulseWidth = activeParams.waveFold;
      globalLfoDepth = activeParams.lfoDepth;

      for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].isSparkle)
          voices[i].setADSR(activeParams.attackTime, 0.1f, 0.7f,
                            activeParams.releaseTime);
        voices[i].setPulseWidth(globalPulseWidth);
      }
      lastParamSync = millis();
    }

    static uint32_t heartbeat = 0;
    if (millis() - heartbeat > 2000) {
      Serial.println("Loop Alive...");
      heartbeat = millis();
    }

    // Input Handling
    if (ts.touched()) {
      if (millis() < inputBlockTimer)
        return;
      TS_Point p = ts.getPoint();
      if (p.z < 600) { // Increased threshold for stability
        if (lastTouchedString != -1) {
          for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].active && voices[v].held && !voices[v].isLatchedArp)
              voices[v].release();
          }
        }
        lastTouchedString = -1;
        return;
      }

      int tx = constrain(
          map(p.x, settings.touch.minX, settings.touch.maxX, 0, 320), 0, 320);
      int ty = constrain(
          map(p.y, settings.touch.minY, settings.touch.maxY, 0, 240), 0, 240);

      // --- TOUCH DISPATCH ---
      if (audioTarget == TARGET_CONFIG) {
        int startY = 60, btnH = 45, btnW = 200, gap = 15;
        // 1. Audio Config (Y=60 to 105)
        if (ty > startY && ty < startY + btnH) {
          audioTarget = TARGET_AUDIO_CONFIG;
          drawAudioConfigScreen();
          delay(250);
          return;
        }
        // 2. Calibration (Y=120 to 165)
        if (ty > startY + (btnH + gap) && ty < startY + (btnH + gap) + btnH) {
          audioTarget = TARGET_CALIBRATION_1;
          drawCalibrationScreen(1);
          delay(500);
          return;
        }
        // 3. Exit (Y=180 to 225)
        if (ty > startY + 2 * (btnH + gap) &&
            ty < startY + 2 * (btnH + gap) + btnH) {
          audioTarget = TARGET_BOOT; // Return to Boot Screen
          drawBootScreen();
          delay(250);
          return;
        }
        return;
      }

      if (audioTarget == TARGET_AUDIO_CONFIG) {
        // Buttons at Y=180, H=40
        if (ty > 180 && ty < 220) {
          // Cycle (x=20, w=100)
          if (tx > 20 && tx < 120) {
            settings.audioProfileIndex++;
            if (settings.audioProfileIndex >= AUDIO_PRESET_COUNT)
              settings.audioProfileIndex = 0;
            drawAudioConfigScreen(); // Redraw with new name
            delay(200);
          }
          // Test Tone (x=140, w=100)
          else if (tx > 140 && tx < 240) {
            // Toggle Test Tone
            isAudioTestRunning = !isAudioTestRunning;
            drawAudioConfigScreen();
            delay(250);
          }
          // OK (x=260, w=310)
          else if (tx > 260 && tx < 310) {
            // Save and Exit
            settings.save();
            audioTarget = TARGET_CONFIG;
            drawConfigMenu();
            delay(250);
          }
        }
        return;
      }

      // Performance Zone
      if (ty < 60) { // Sound Design Bar (Upper Bound 60px)
        static uint32_t lastTopPress = 0;
        if (millis() - lastTopPress > 300) {
          if (tx < 53) {
            if (delayPressStart == 0)
              delayPressStart = millis();
            if (millis() - delayPressStart > 2000) {
              // Long press logic removed (was LED toggle)
              delayPressStart = 0;
              delay(500);
            }
          } else if (tx < 106) {
            fxDrive = !fxDrive;
            drawFXButtons();
            lastTopPress = millis();
          } else if (tx < 159) {
            fxTrem = !fxTrem;
            drawFXButtons();
            lastTopPress = millis();
          } else if (tx < 212) {
            fxLFO = !fxLFO;
            drawFXButtons();
            lastTopPress = millis();
          } else if (tx < 265) {
            if (!waitForArpRelease) {
              if (arpPressStart == 0)
                arpPressStart = millis();
              if (millis() - arpPressStart > 500) {
                if (!arpLatch) {
                  arpLatch = true;
                  latchedOctaveShift = octaveShift;
                  drawArpButton();
                  arpPressStart = 0;
                  waitForArpRelease = true;
                }
              }
            }
          } else {
            if (wavePressStart == 0)
              wavePressStart = millis();
            drawWaveButton();
            if (millis() - wavePressStart > 250) {
              currentMode = MODE_EDIT;
              editorInputBlocked = true;
              tft.fillScreen(COLOR_BG);
              drawEditor();
              wavePressStart = 0;
              delay(500);
              return;
            }
          }
        }
      } else if (ty >= 190) { // Presets (Taller)
        int btnIdx = tx / (SCREEN_WIDTH / 8);
        static uint32_t lastBtnPress = 0;
        if (millis() - lastBtnPress > 200) {
          handleButtonPress(btnIdx);
          lastBtnPress = millis();
        }
      } else if (ty >= 140 && ty < 190) { // Sliders (Area shifted and taller)
        if (tx < 240) {
          masterVolume = (float)tx / 240.0f;
          drawVolumeSlider();
        } else {
          static uint32_t lastTransPress = 0;
          if (millis() - lastTransPress > 250) {
            rootNote = (rootNote + 1) % 12;
            drawTransposeButton();
            drawStrings();
            lastTransPress = millis();
          }
        }
      } else if (ty >= 60 && ty < 140) { // Strings (Area reduced)
        int numStrings = activeParams.octaveRange * 12 + 1;

        // Offset Logic for Centering Middle C
        int offset = getVisualStringOffset();
        int sIdxRaw = (tx * numStrings) / SCREEN_WIDTH;
        int sIdx = sIdxRaw + offset;

        // OOB Check
        if (sIdx >= STRING_COUNT)
          sIdx = STRING_COUNT - 1;
        if (sIdx < 0)
          sIdx = 0;

        if (sIdx != lastTouchedString) {
          if (lastTouchedString != -1) {
            for (int v = 0; v < MAX_VOICES; v++) {
              if (voices[v].active && voices[v].held && !voices[v].isLatchedArp)
                voices[v].release();
            }
          }
          if (arpMode != ARP_OFF && !arpLatch) {
            if (currentChordMask & (1 << (sIdx % 12)))
              fireArp((sIdx / 12) * 12);
          } else {
            triggerNote(sIdx);
          }
          lastTouchedString = sIdx;
        }
      }
    } else { // No Touch
      waitForArpRelease = false;
      if (arpPressStart > 0) {
        static uint32_t lastArpRelease = 0;
        if (millis() - lastArpRelease > 300) { // Debounce release
          if (arpLatch) {
            arpMode = ARP_OFF;
            arpLatch = false;
            for (int v = 0; v < MAX_VOICES; v++)
              if (voices[v].isLatchedArp)
                voices[v].release();
          } else if (millis() - arpPressStart > 500) {
            arpLatch = true;
          } else {
            arpMode = (ArpMode)((int)arpMode + 1);
            if ((int)arpMode > 7)
              arpMode = ARP_OFF;
            if (arpMode == ARP_RANDOM)
              shuffleArpSequence();
          }
          drawArpButton();
          lastArpRelease = millis();
        }
        arpPressStart = 0;
      }
      if (wavePressStart > 0) {
        static uint32_t lastWaveRelease = 0;
        if (millis() - lastWaveRelease > 300) { // Debounce release
          if (millis() - wavePressStart < 1500) {
            selectWaveform((currentWaveform + 1) % 4);
            drawWaveButton();
          }
          lastWaveRelease = millis();
        }
        wavePressStart = 0;
      }
      if (delayPressStart > 0) {
        static uint32_t lastDelayRelease = 0;
        if (millis() - lastDelayRelease > 300) {
          delayMode = (delayMode + 1) % 5;
          drawDelayButton();
          lastDelayRelease = millis();
        }
        delayPressStart = 0;
      }
      if (lastTouchedString != -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].held && !voices[v].isLatchedArp)
            voices[v].release();
        }
        lastTouchedString = -1;
      }
    }
  }
}

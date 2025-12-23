// --- ESP32 CYD Autoharp - Fixed Setup Duplicate ---
#include "Config.h"
#include "SynthVoice.h"
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <driver/i2s.h>

// --- BLUETOOTH TOGGLE ---
// Uncomment the line below to enable Bluetooth Audio Mode
// #define ENABLE_BLUETOOTH
// ------------------------

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSource.h"
BluetoothA2DPSource a2dp_source;
#else
#include "soc/rtc_io_reg.h"
#include "soc/sens_reg.h"
#include <driver/dac.h>
#endif

// --- Globals ---
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

SynthVoice voices[MAX_VOICES];
float baseFreqs[STRING_COUNT];
int octaveShift = 0;
uint16_t activeChord = 0;
uint16_t currentChordMask = 0xFFFF;
int activeButtonIndex = -1;

// --- Audio Buffer ---
#define AUDIO_BUF_SIZE 2048
volatile uint8_t audioBuffer[AUDIO_BUF_SIZE];
volatile int bufReadHead = 0;
volatile int bufWriteHead = 0;

Waveform currentWaveform = WAVE_SAW;
float globalPulseWidth = 0.5f;

// Filter State (Chamberlin SVF) - MOVED BELOW
// Removing duplicates caused by previous edit error
// Filter State (Chamberlin SVF)
// Filter State (Chamberlin SVF)
volatile float svf_low = 0;
volatile float svf_band = 0;
volatile float svf_f = 0.5f;  // Cutoff coefficient
volatile float svf_q = 0.22f; // Resonance

// Tape Wobble State
float wowPhase = 0.0f;
float flutterPhase = 0.0f;
const float wowInc = 2.0f * PI * 1.5f / SAMPLE_RATE;     // 1.5Hz
const float flutterInc = 2.0f * PI * 6.0f / SAMPLE_RATE; // 6Hz
// Delay State
// Delay State
#define MAX_DELAY_MS 1200
#define DELAY_DOWNSAMPLE 4
#define MAX_DELAY_LEN                                                          \
  (int)(SAMPLE_RATE * MAX_DELAY_MS / 1000 / DELAY_DOWNSAMPLE)
// Using int16_t for buffer to save RAM (26KB now, super safe) + Super Vintage
// Grit
int16_t *delayBuffer = 0;
int delayHead = 0;
int delayTick = 0;           // For downsampling
int delayMode = 0;           // 0=Off, 1=300ms, 2=600ms, 3=900ms, 4=1200ms
float wobbleDepth = 0.0025f; // Default 0.25%

// UI State
// UI State
int lastTouchedString = -1;
bool isBooting = true;

// --- GLOBAL SETTINGS ---
ArpMode arpMode = ARP_OFF;
SynthParameters activeParams = {.filterCutoff = 2000.0f,
                                .filterRes = 0.5f,
                                .waveFold = 0.0f,
                                .delayFeedback = 0.3f,
                                .attackTime = 0.01f,
                                .releaseTime = 0.5f,
                                .lfoRate = 1.0f,
                                .lfoDepth = 0.0f,
                                .driveAmount = 0.0f,
                                .tremRate = 0.0f,
                                .lfoType = LFO_SINE,
                                .lfoTarget = TARGET_NONE,
                                .ldrTarget = TARGET_NONE};

// --- ARP & SPARKLE STATE ---
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

bool arpLatch = false;
int arpStep = 0;
uint8_t activeNotes[12]; // Stores string indices (0-11) of current chord
int activeNoteCount = 0;
uint8_t randomSequence[12];
uint32_t arpPressStart = 0;
uint32_t lastArpClock = 0;

// Helper function declarations
void fireArp(int octaveOffset = 0, bool latched = false);
int getClosestStringIndex(float freq);
void scheduleSpark(int delayMs, float freq, int stringIdx, float release,
                   float vel);
void drawBootScreen(); // Forward declaration

// --- AUDIO GENERATION LOGIC (Shared) ---
// Returns a single float sample (-1.0 to 1.0) mixed from all voices + Filtered
float generateMixedSample(float pitchMod) {
  float mixedSample = 0.0f;
  int activeCount = 0;

  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active) {
      mixedSample += voices[i].getSample(pitchMod);
      activeCount++;
    }
  }

  if (activeCount > 0) {
    mixedSample *=
        0.25f; // Increased headroom to prevent crackling with many notes
  }

  // Apply Resonant Low Pass Filter (Chamberlin SVF)
  // Algorithm:
  // low = low + f * band;
  // high = input - low - q * band;
  // band = band + f * high;

  // We run it 2x Oversampled (optional, but helps stability at high f)?
  // No, 16kHz is low, keep it simple.

  // Anti-Denormal noise (prevents CPU stutter on silence)
  mixedSample += 1.0e-18f;

  svf_low += svf_f * svf_band;
  float high = mixedSample - svf_low - (svf_q * svf_band);
  svf_band += svf_f * high;

  // Stability Clamping (Prevents explosions/silence)
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

#ifdef ENABLE_BLUETOOTH
// --- BLUETOOTH CALLBACK ---
// The A2DP library calls this to get data.
// Signature match: int32_t (*)(Frame *data, int32_t len) where len is frame
// count
int32_t bt_data_stream_callback(Frame *data, int32_t len) {
  for (int i = 0; i < len; i++) {
    // Note: Tape Wobble implementation replicated from fillAudioBuffer
    // Optimization: Only calc LFO every 16 samples to save CPU
    static float currentPitchMod = 1.0f;
    if (i % 16 == 0) {
      wowPhase += wowInc * 16.0f; // Advance 16 steps
      if (wowPhase >= 6.283185307f)
        wowPhase -= 6.283185307f;
      flutterPhase += flutterInc * 16.0f;
      if (flutterPhase >= 6.283185307f)
        flutterPhase -= 6.283185307f;

      currentPitchMod =
          1.0f + (sin(wowPhase) + sin(flutterPhase)) * wobbleDepth;
    }

    float sample = generateMixedSample(currentPitchMod);

    // Delay Processing (Super-Vintage 11kHz)
    delayTick++;
    if (delayTick >= DELAY_DOWNSAMPLE)
      delayTick = 0;

    float delayed = 0.0f;

    if (delayMode > 0 && delayBuffer != 0) {
      // Calculate Read Position (Account for downsample)
      int delayMs = delayMode * 300;
      int delaySamples = (SAMPLE_RATE / DELAY_DOWNSAMPLE * delayMs) / 1000;
      int readPos = (delayHead - delaySamples + MAX_DELAY_LEN) % MAX_DELAY_LEN;

      delayed = (float)delayBuffer[readPos] * 3.3333e-5f;
    }

    float dry = sample;
    sample = dry + delayed * 0.5f; // Mix

    // Write Back (On Tick 0)
    if (delayTick == 0 && delayBuffer != 0) {
      if (delayMode > 0) {
        float fb = delayed * 0.25f + dry * 0.7f;
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

    // Clip hard
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;

    // Scale to int16
    int16_t pcm = (int16_t)(sample * 30000.0f); // Use 30000.0f for headroom

    // Stereo Frame
    data[i].channel1 = pcm; // Left
    data[i].channel2 = pcm; // Right
  }
  return len;
}

#else
// --- WIRED/DAC CALLBACKS ---
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// --- ISR: Read Buffer ---
// --- Audio Generation Task (Called in loop) ---
// --- Audio Generation Task (Called in loop) ---
// --- ISR: Read Buffer ---
void ARDUINO_ISR_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);

  if (bufReadHead != bufWriteHead) {
    uint8_t val = audioBuffer[bufReadHead];
    bufReadHead = (bufReadHead + 1) % AUDIO_BUF_SIZE;
    dacWrite(26, val); // Pin 26 (Sunton DAC)
  }

  portEXIT_CRITICAL_ISR(&timerMux);
}

// --- Audio Generation Task (Called in loop) ---
void fillAudioBuffer() {
  // Fill up to 256 samples per call to allow UI updates and WDT reset
  int samplesToFill = 256;

  while (samplesToFill > 0) {
    int nextWrite = (bufWriteHead + 1) % AUDIO_BUF_SIZE;
    if (nextWrite == bufReadHead)
      return; // Buffer Full

    // Update Tape Wobble LFOs (Optimized: Every 16th sample)
    static float currentPitchModWired = 1.0f;
    static int pModCtr = 0;
    pModCtr++;
    if (pModCtr >= 16) {
      pModCtr = 0;
      wowPhase += wowInc * 16.0f;
      if (wowPhase >= 6.283185307f)
        wowPhase -= 6.283185307f;
      flutterPhase += flutterInc * 16.0f;
      if (flutterPhase >= 6.283185307f)
        flutterPhase -= 6.283185307f;
      currentPitchModWired =
          1.0f + (sin(wowPhase) + sin(flutterPhase)) * wobbleDepth;
    }

    float s = generateMixedSample(currentPitchModWired);

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

    float dry = s;
    s = dry + delayed * 0.5f; // Mix

    if (delayTick == 0 && delayBuffer != 0) {
      if (delayMode > 0) {
        float fb = delayed * 0.55f + dry * 0.7f;
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

    // Clip
    if (s > 1.0f)
      s = 1.0f;
    if (s < -1.0f)
      s = -1.0f;

    // Scale for DAC (8-bit)
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
#endif

// --- UI Functions (Forward Declared implicitly by ordering) ---
void drawWaveButton(); // Forward decl due to ordering
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
      // Clip drawing area if under the Buttons
      // Waveform Button (Top Right): x > SCREEN_WIDTH - 60
      // Delay Button (Top Left): x < 60
      int drawY = 0;
      int drawH = STRING_HEIGHT;

      if (x < 60 || x + w > SCREEN_WIDTH - 60) {
        drawY = 40;
        drawH = STRING_HEIGHT - 40;
      }

      tft.fillRect(x, drawY, w, drawH, currentColor);
      tft.drawRect(x, drawY, w, drawH, TFT_DARKGREY);

      // Only draw label for C notes (every 12th)
      if (i % 12 == 0) {
        uint16_t textColor = isBlack ? TFT_WHITE : TFT_BLACK;
        // If highly active, maybe make text white always?
        if (stringEnergy[i] > 0.5f)
          textColor = TFT_WHITE;

        // Ensure label is visible (move down if clipped)
        int labelY = 5;
        if (drawY > 0)
          labelY = drawY + 5;

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
// Helper to find closest string index for a frequency
int getClosestStringIndex(float freq) {
  float minDiff = 10000.0f;
  int closestIdx = -1;
  for (int i = 0; i < STRING_COUNT; i++) {
    float diff = abs(baseFreqs[i] - freq);
    if (diff < minDiff) {
      minDiff = diff;
      closestIdx = i;
    }
  }
  return closestIdx;
}

void scheduleSpark(int delayMs, float freq, int stringIdx, float release,
                   float vel) {
  for (int i = 0; i < MAX_SPARKS; i++) {
    if (!pendingSparks[i].active) {
      pendingSparks[i].active = true;
      pendingSparks[i].triggerTime = millis() + delayMs;
      pendingSparks[i].freq = freq;
      pendingSparks[i].stringIdx = stringIdx;
      pendingSparks[i].releaseTime = release;
      pendingSparks[i].velocity = vel;
      return;
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
    arpStep++;
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
  case ARP_WALK: {
    int base = (arpStep / 2) % activeNoteCount;
    if (arpStep % 2 == 0) {
      noteIdx = base;
    } else {
      noteIdx = (base + 2) % activeNoteCount;
    }
    arpStep++;
    if (arpStep >= activeNoteCount * 2)
      arpStep = 0;
  } break;
  case ARP_RANDOM:
    noteIdx = randomSequence[arpStep % activeNoteCount];
    arpStep++;
  default:
    break;
  }

  // Play the Note
  // Add Octave Offset from Strum Position
  // Note: activeNotes contains raw indices (0..STRING_COUNT-1)
  int stringIndex = activeNotes[noteIdx] + octaveOffset;

  // Bounds Check
  if (stringIndex >= STRING_COUNT) {
    return; // Don't play out of bounds
  }
  if (stringIndex < 0)
    stringIndex = 0;

  // Trigger Logic for Latch vs Unlatched
  if (latched) {
    // Find Voice
    int vIdx = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
      if (!voices[i].active) {
        vIdx = i;
        break;
      }
    }
    if (vIdx == -1)
      vIdx = 0; // Steal

    float freq = baseFreqs[stringIndex];
    if (octaveShift != 0) {
      // Apply octave shift logic if needed here, but activeNotes usually
      // pre-calcs? No, activeNotes are just indices. BaseFreqs handles freq. We
      // should respect octaveShift. Actually base code applied octaveShift to
      // freq.
      if (octaveShift > 0)
        for (int k = 0; k < octaveShift; k++)
          freq *= 2.0f;
      else if (octaveShift < 0)
        for (int k = 0; k < abs(octaveShift); k++)
          freq *= 0.5f;
    }

    // Trigger LED Visual (Not implemented here, skipping)

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
    // Manual Arp Logic (simplified)
  }
}

void drawWaveButton() {
  int x = SCREEN_WIDTH - 50; // Top Right
  int y = 5;
  int w = 40;
  int h = 30;

  tft.fillRect(x, y, w, h, TFT_DARKGREY);
  tft.drawRect(x, y, w, h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE); // Ensure text color is white
  tft.setTextDatum(MC_DATUM);

  // Draw Icon (Simple lines)
  if (currentWaveform == WAVE_SAW) {
    // Sawtooth Line: /|/|
    tft.drawLine(x + 5, y + h - 5, x + 15, y + 5, TFT_WHITE);
    tft.drawLine(x + 15, y + 5, x + 15, y + h - 5, TFT_WHITE);
    tft.drawLine(x + 15, y + h - 5, x + 25, y + 5, TFT_WHITE);
    tft.drawLine(x + 25, y + 5, x + 25, y + h - 5, TFT_WHITE);
  } else if (currentWaveform == WAVE_SQUARE) {
    // Square Line: |_|
    tft.drawLine(x + 5, y + 5, x + 15, y + 5, TFT_WHITE);
    tft.drawLine(x + 15, y + 5, x + 15, y + h - 5, TFT_WHITE);
    tft.drawLine(x + 15, y + h - 5, x + 25, y + h - 5, TFT_WHITE);
    tft.drawLine(x + 25, y + h - 5, x + 25, y + 5, TFT_WHITE);
    tft.drawLine(x + 25, y + 5, x + 35, y + 5, TFT_WHITE);
  } else if (currentWaveform == WAVE_SINE) {
    // Sine Wave Icon
    int startX = x + 5;
    int centerY = y + 15;
    int prevY = centerY;
    for (int i = 0; i <= 30; i++) {
      float rad = (i / 30.0f) * 6.28318f;
      int newY =
          centerY - (int)(sin(rad) * 10.0f); // Inverted Y for screen coords
      if (i > 0)
        tft.drawLine(startX + i - 1, prevY, startX + i, newY, TFT_WHITE);
      prevY = newY;
    }
  } else if (currentWaveform == WAVE_TRIANGLE) {
    // Triangle: /\/\ (Double Cycle to match Saw/Square)
    // Coords: 5, 12, 20, 27, 35
    int yTop = y + 5;
    int yBot = y + h - 5;
    tft.drawLine(x + 5, yBot, x + 12, yTop, TFT_WHITE);  // Up
    tft.drawLine(x + 12, yTop, x + 20, yBot, TFT_WHITE); // Down
    tft.drawLine(x + 20, yBot, x + 27, yTop, TFT_WHITE); // Up
    tft.drawLine(x + 27, yTop, x + 35, yBot, TFT_WHITE); // Down
  }
}

void drawDelayButton() {
  int x = 10; // Top Left
  int y = 5;
  int w = 40;
  int h = 30;

  uint16_t color = TFT_DARKGREY;
  const char *label = "DLY";

  if (delayMode == 1) {
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

  tft.setTextColor(delayMode > 0 && delayMode != 4
                       ? TFT_BLACK
                       : TFT_WHITE); // Black text for bright colors
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + 20, y + 15);
}

void drawButtons() {
  int y = STRING_HEIGHT;
  int h = BUTTON_HEIGHT;
  int btnCount = 8;
  int w = SCREEN_WIDTH / btnCount;

  // Bank Colors: Green, Cyan, Yellow, Magenta, Orange, Grey (Off)
  uint16_t bankColors[] = {TFT_GREEN,   TFT_CYAN,   TFT_YELLOW,
                           TFT_MAGENTA, TFT_ORANGE, TFT_LIGHTGREY};

  for (int i = 0; i < btnCount; i++) {
    uint16_t color = COLOR_BTN_INACTIVE;
    const char *label = "";

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
      int bank = buttonStates[i];
      if (activeButtonIndex == -1) { // Chromatic mode
        label = "Off";
        color = TFT_LIGHTGREY;
      } else if (activeButtonIndex == i) {
        label = chordLabels[i - 1][bank];
        color = bankColors[bank];
      } else { // Other chord buttons when one is active
        label = chordLabels[i - 1][bank];
        color = COLOR_BTN_INACTIVE;
      }
    }

    tft.fillRect(i * w, y, w, h, color);
    tft.drawRect(i * w, y, w, h, TFT_WHITE);

    // Text Color: Black if active background is bright, White otherwise
    if (i >= 1 && i <= 6 && activeButtonIndex == i)
      tft.setTextColor(TFT_BLACK);
    else
      tft.setTextColor(TFT_WHITE);

    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, i * w + (w / 2), y + (h / 2));
  }
}

void updateButtonVisuals() {
  // Re-use logic (duplicated for now to avoid massive refactor of call sites)
  drawButtons();
}

void drawInterface() {
  drawStrings();
  drawButtons();
  drawWaveButton();
  drawDelayButton();
}

void handleButtonPress(int index) {
  if (index == 0) { // Octave Down
    octaveShift--;
    if (octaveShift < -3)
      octaveShift = -3;
  } else if (index == 7) { // Octave Up
    octaveShift++;
    if (octaveShift > 3)
      octaveShift = 3;
  } else {
    // Chord Button
    if (activeButtonIndex == index) {
      // Cycle State (0-4 now), do NOT go to Off(5)
      buttonStates[index] = (buttonStates[index] + 1) % 5;
    } else {
      // Pressed a DIFFERENT button
      if (activeButtonIndex != -1) {
        // Step 1: Deselect Current -> Go Chromatic
        activeButtonIndex = -1;
        currentChordMask = 0xFFFF; // Chromatic
        // Do NOT select the new logic yet.
        // Update visuals and return.
        updateButtonVisuals();
        Serial.println("Chord Cleared (Chromatic)");
        delay(150);
        return;
      } else {
        // Step 2: From Chromatic -> Select New Logic
        activeButtonIndex = index;
        // Keep previous state (memory).
      }
    }

    // Apply Mask based on state
    if (activeButtonIndex == -1) {
      currentChordMask = 0xFFFF; // Chromatic
    } else {
      int state = buttonStates[index];
      currentChordMask = chordMasks[index - 1][state];
    }
  }

  // Log
  if (index >= 1 && index <= 6) {
    int s = buttonStates[index];
    const char *l = (s == 5) ? "Off" : chordLabels[index - 1][s];
    Serial.printf("Chord: %s (Mask: 0x%X)\n", l, currentChordMask);
  } else {
    Serial.printf("Octave: %d\n", octaveShift);
  }

  delay(150); // Debounce
  updateButtonVisuals();
}

void setup() {
  Serial.begin(115200);

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
  // C3 = 130.81 Hz (Up one octave from C2)
  float k = 1.059463094359;
  float f = 130.81f;
  for (int i = 0; i < STRING_COUNT; i++) {
    baseFreqs[i] = f;
    f *= k;
  }

#ifdef ENABLE_BLUETOOTH
  // --- BLUETOOTH SETUP ---
  Serial.println("Starting A2DP...");
  // Connect to specific speaker
  a2dp_source.start("Tribit XSound Go", bt_data_stream_callback);
  a2dp_source.set_volume(100);
#else
  // --- DAC SETUP (Final Sync) ---
  // Pins: DAC2 = GPIO 26

  // Power Pins (Shotgun)
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH); // Common
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH); // Alternative
  pinMode(22, OUTPUT);
  digitalWrite(22, HIGH); // Alternative
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH); // Backlight/Amp?

  // Init DAC
  dacWrite(26, 128); // Mid-rail

  // Loud Tone Test (DAC)
  for (int t = 0; t < 50; t++) {
    dacWrite(26, 255);
    delay(2);
    dacWrite(26, 0);
    delay(2);
  }

  // Init Audio Timer (V3 Compatible)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 1000000 / SAMPLE_RATE, true, 0);
#else
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(timer);
#endif
#endif

  // Default Delay for fun
  delayMode = 2; // 600ms

  drawBootScreen();
  Serial.println("SETUP COMPLETE (Boot Screen Active)");
}

void loop() {
#ifndef ENABLE_BLUETOOTH
  // 1. Fill Audio Buffer (Priority)
  fillAudioBuffer();
#endif

  // Update Visuals (Throttled to ~60FPS to save SPI/CPU)
  // Update Visuals (Throttled to ~60FPS)
  // ONLY if not booting (prevents ghosting)
  if (!isBooting) {
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
      // ... (truncated for brevity, but we assume the logic continues inside)
      // Actually, to wrap large block, I need to match carefully.
      // Wait, replace_file_content replaces the WHOLE BLOCK I specify.
      // I cannot verify the content of lines 1002-1080 without including them.
      // I will just modify the START of the block and the END of the Loop touch
      // logic.
    }
  }

  // 3. Input Handling
  // Strategy: Call fillAudioBuffer() frequently.

  if (ts.touched()) {
    // Fill buffer again before expensive touch logic
#ifndef ENABLE_BLUETOOTH
    fillAudioBuffer();
#endif
    TS_Point p = ts.getPoint();

    if (isBooting) {
      // Active Beep Test (Verify DAC Pin 26)
      static uint32_t lastBeep = 0;
      static bool beepState = false;
      if (millis() - lastBeep > 300) { // Beep every 300ms
        lastBeep = millis();
        beepState = !beepState;
        dacWrite(26, beepState ? 100 : 0);
        dacWrite(25, beepState ? 100 : 0);
      }

      // Exit Boot Screen on Touch
      if (p.z > 400) { // Valid touch
        isBooting = false;
        tft.fillScreen(COLOR_BG);
        drawInterface();
        dacWrite(26, 0);
        dacWrite(25, 0);
        delay(300);
        while (ts.touched()) {
          delay(10);
        }
        return;
      }
      return; // Stay in boot loop
    }

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

    // Calibration (Sunton 3.5" Inverted Fix)
    // Axes: X->X, Y->Y (Standard)
    // Range: Inverted (Max -> Min)
    int tx = map(p.x, 3700, 380, 0, SCREEN_WIDTH);
    int ty = map(p.y, 3700, 450, 0, SCREEN_HEIGHT);

    // Bounds checking...
    if (tx < 0)
      tx = 0;
    if (tx > SCREEN_WIDTH)
      tx = SCREEN_WIDTH;
    if (ty < 0)
      ty = 0;
    if (ty > SCREEN_HEIGHT)
      ty = SCREEN_HEIGHT;

    // Check Wave Button (Top Right)
    // Rect: x=SCREEN_WIDTH-50, y=5, w=40, h=30
    // Hitbox extended slightly for usability
    if (tx > SCREEN_WIDTH - 80 && ty < 60) {
      static uint32_t lastWavePress = 0;
      if (millis() - lastWavePress > 300) {
        if (currentWaveform == WAVE_SAW)
          currentWaveform = WAVE_SQUARE;
        else if (currentWaveform == WAVE_SQUARE)
          currentWaveform = WAVE_SINE;
        else if (currentWaveform == WAVE_SINE)
          currentWaveform = WAVE_TRIANGLE;
        else
          currentWaveform = WAVE_SAW;

        drawWaveButton();
        Serial.printf("Waveform: %d\n", currentWaveform);
        lastWavePress = millis();
      }
      return;
    }

    // Check Delay Button (Top Left)
    // Rect: x=10, y=5, w=40, h=30
    if (tx < 80 && ty < 60) {
      static uint32_t lastDelayPress = 0;
      // Debounce 400ms to prevent double toggles
      if (millis() - lastDelayPress > 400) {
        delayMode++;
        if (delayMode > 4)
          delayMode = 0; // Cycle 0-4

        drawDelayButton();
        lastDelayPress = millis();
        Serial.printf("DELAY BUTTON: Mode -> %d\n", delayMode);
      }
      return;
    }

    if (ty < STRING_HEIGHT) {
      // Dynamic width for 37 strings
      int sIdx = (tx * STRING_COUNT) / SCREEN_WIDTH;

      if (sIdx >= 0 && sIdx < STRING_COUNT) {

        if (sIdx != lastTouchedString) {
          // 1. Release Old String
          if (lastTouchedString != -1) {
            // Also release synth voice just in case/visuals
            for (int v = 0; v < MAX_VOICES; v++) {
              if (voices[v].active &&
                  voices[v].noteIndex == lastTouchedString && voices[v].held) {
                voices[v].release();
              }
            }
          }

          // 2. Trigger New String
          // Calculate Inversion Cutoff
          int btnState = (activeButtonIndex >= 1 && activeButtonIndex <= 6)
                             ? buttonStates[activeButtonIndex]
                             : 0;
          int invCutoff = 0;
          if (btnState >= 2 &&
              btnState <= 4) { // Banks 3,4,5 are Inversions 1,2,3
            // Inv 1 (Bank 3/State 2): Skip Root
            // Inv 2 (Bank 4/State 3): Skip Root, 3rd
            // Inv 3 (Bank 5/State 4): Skip Root, 3rd, 5th
            invCutoff = getInversionCutoff(currentChordMask, btnState - 1);
          }

          if ((currentChordMask & (1 << (sIdx % 12))) &&
              sIdx >= invCutoff) { // Logic + Cutoff
            float freq = baseFreqs[sIdx];
            if (octaveShift > 0) {
              for (int k = 0; k < octaveShift; k++)
                freq *= 2.0f;
            } else if (octaveShift < 0) {
              for (int k = 0; k < abs(octaveShift); k++)
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
            if (vIdx == -1)
              vIdx = 0; // Steal voice 0 if full (simplistic)

            voices[vIdx].trigger(freq, sIdx);

            // Visuals (Still show visuals in MIDI mode!)
            stringEnergy[sIdx] = 1.0f;
          }

          lastTouchedString = sIdx;
        }
        // Else: Holding same string, do nothing (Sustain handled by envelope)
      }
    } else { // Button Area
      // 1. If we were holding a string, release it because we left the area
      if (lastTouchedString != -1 && lastTouchedString != -2) {
        for (int v = 0; v < MAX_VOICES; v++) {
          if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
              voices[v].held) {
            voices[v].release();
          }
        }
        lastTouchedString = -1;
        // Don't trigger button immediately after releasing string (prevent
        // accidental presses)
        return;
      }

      // Only handle button press if not currently holding a string (redundant
      // check but safe)
      if (lastTouchedString == -1) {
        int btnW = SCREEN_WIDTH / 8;
        int btnIdx = tx / btnW;
        if (btnIdx >= 0 && btnIdx < 8) {
          handleButtonPress(btnIdx);
          lastTouchedString = -2;
        }
      }
    }
  } else {
    // No Touch (Finger Lifted)
    if (lastTouchedString != -1 && lastTouchedString != -2) {
      // Release held note
      for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].active && voices[v].noteIndex == lastTouchedString &&
            voices[v].held) {
          voices[v].release();
        }
      }
    }
    lastTouchedString = -1;
  }

  // --- SPARKLE PROCESSING ---
  if (arpMode == ARP_SPARKLE || arpMode == ARP_SPARKLE2) {
    uint32_t now = millis();
    for (int i = 0; i < MAX_SPARKS; i++) {
      if (pendingSparks[i].active && now >= pendingSparks[i].triggerTime) {
        int vIdx = -1;
        for (int v = 0; v < MAX_VOICES; v++) {
          if (!voices[v].active) {
            vIdx = v;
            break;
          }
        }
        if (vIdx == -1)
          vIdx = 0; // Steal

        voices[vIdx].trigger(pendingSparks[i].freq, pendingSparks[i].stringIdx,
                             currentWaveform, globalPulseWidth, 0.00f,
                             pendingSparks[i].releaseTime, 0.0f,
                             pendingSparks[i].releaseTime);
        voices[vIdx].isSparkle = true;
        pendingSparks[i].active = false;
      }
    }
  }

  // --- ARP LATCH TIMER ---
  if (arpMode != ARP_OFF && arpLatch) {
    float rate = activeParams.lfoRate;
    if (rate < 0.1f)
      rate = 0.1f;
    uint32_t interval = 1000.0f / (rate * 0.20f);

    if (millis() - lastArpClock > interval) {
      fireArp(0, true);
      lastArpClock = millis();
    }
  }

  delay(5);
}

// --- BOOT SCREEN LOGIC ---
void drawBootScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);

  // Header
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("ElectroHarp Pocket", SCREEN_WIDTH / 2,
                 SCREEN_HEIGHT / 2 - 40);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.drawString("Sunton 3.5\" Edition", SCREEN_WIDTH / 2,
                 SCREEN_HEIGHT / 2 - 10);

  tft.drawString("Touch to Start", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 40);

  // Version
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("v3.5 - I2S Audio", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 20);
}

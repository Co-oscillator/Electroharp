// --- ESP32 CYD Autoharp - Fixed Setup Duplicate ---
#include "Config.h"
#include "SynthVoice.h"
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// --- BLUETOOTH TOGGLE ---
// Uncomment the line below to enable Bluetooth Audio Mode
#define ENABLE_BLUETOOTH
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
int lastTouchedString = -1;

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

  portEXIT_CRITICAL_ISR(&timerMux);
}

// --- Audio Generation Task (Called in loop) ---
void fillAudioBuffer() {
  // Fill up to 256 samples per call to allow UI updates and WDT reset
  // Increased from 64 to prevent underruns during visual blocking
  int samplesToFill = 256;

  while (samplesToFill > 0) {
    int nextWrite = (bufWriteHead + 1) % AUDIO_BUF_SIZE;
    if (nextWrite == bufReadHead)
      return; // Buffer Full

    // Update Tape Wobble LFOs (Optimized: Every 16th sample)
    // We can't easily skip loop iterations here because we need to fill buffer
    // linearly. So we just update pitchMod occasionally.
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

    // Delay Processing (Duplicate logic for DAC)
    // Note: We share the same buffer/head. This is risky if both run, but BT
    // usually exclusive. If both run, head advances 2x. Assuming #ifdef
    // ENABLE_BLUETOOTH handles exclusivity or we use separate indices? User
    // code structure implies exclusive usage via #ifndef ENABLE_BLUETOOTH in
    // loop. But let's be safe: If BT is enabled, fillAudioBuffer shouldn't run?
    // Code says: #ifndef ENABLE_BLUETOOTH fillAudioBuffer(); #endif
    // So it's exclusive. Safe to share globals.
    // Delay Processing (Super-Vintage - Duplicate Logic)
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
  // Clear buffer
  for (int i = 0; i < AUDIO_BUF_SIZE; i++)
    audioBuffer[i] = 128;

  // Init DAC hardware (Enable output)
  dacWrite(AUDIO_PIN, 0);

  // Init Audio Timer
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(timer);
#endif

  drawInterface();
  Serial.println("SETUP COMPLETE");
}

void loop() {
#ifndef ENABLE_BLUETOOTH
  // 1. Fill Audio Buffer (Priority)
  fillAudioBuffer();
#endif

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

    // Widen Range: 0 (Dark) to 2000 (Bright)
    int constrainedLdr = constrain((int)smoothedLdr, 0, 2000);

    // Inverse LDR Logic (Hardware is Pull-Up: Bright = Low Value)
    // Input: 0 (Bright) -> Output 1000 (1.0)
    ldrNorm = map(constrainedLdr, 0, 2000, 1000, 0) / 1000.0f;

    // Optimization: Skip expensive math/updates if LDR hasn't changed much
    static float lastLdrNorm = -1.0f;
    if (abs(ldrNorm - lastLdrNorm) > 0.002f) {
      lastLdrNorm = ldrNorm;

      if (currentWaveform == WAVE_SAW) {
        // Default: LDR -> Filter Freq, Q = 0.10 (Resonance)
        float minF = 500.0f; // User Requested Floor 500Hz
        float maxF =
            11000.0f; // Limit to 11kHz (Fs/3) for stability. 16kHz is unsafe.

        // Sensitivity Adjustment: Squared curve (User Request)
        float ldrCurve = ldrNorm * ldrNorm; // Optimized pow(x, 2)

        float targetFreq = minF * pow(maxF / minF, ldrCurve);

        float targetF = 2.0f * sin(PI * targetFreq / (float)SAMPLE_RATE);

        // Safety Clamp: Chamberlin filter explodes if F > 0.8 (or near 1.0)
        // With High Polyphony, limit to 0.5 to prevent overflow/underruns.
        if (targetF > 0.6f)
          targetF = 0.6f;
        if (targetF < 0.005f)
          targetF = 0.005f;

        svf_f = svf_f * 0.9f + targetF * 0.1f;
        svf_q = 0.20f; // Stabilized resonance (was 0.10)

      } else if (currentWaveform == WAVE_SQUARE) {
        // Square: LDR -> Pulse Width (0.12 - 0.5)
        float targetPW = 0.12f + ((1.0f - ldrNorm) * 0.38f);
        globalPulseWidth = globalPulseWidth * 0.9f + targetPW * 0.1f;

        svf_f = 0.6f; // Fully Open (actually 0.6 now)
        svf_q = 0.5f; // Flat

      } else if (currentWaveform == WAVE_SINE) {
        // Sine: LDR -> Wobble Depth
        float targetDepth = 0.0f;
        if (ldrNorm > 0.8f) {
          targetDepth = 0.0f;
        } else {
          // Scale 0.8 (0%) to 0.0 (2.75%)
          float cover = (0.8f - ldrNorm) / 0.8f;
          targetDepth = cover * 0.0275f;
        }
        wobbleDepth = wobbleDepth * 0.9f + targetDepth * 0.1f;

        svf_f = 0.95f; // Filter Open (95%)
        svf_q = 0.5f;  // Flat
      } else if (currentWaveform == WAVE_TRIANGLE) {
        // Triangle: LDR -> Wavefolder (Same as sine)
        float targetFold = ldrNorm;
        globalPulseWidth = globalPulseWidth * 0.9f + targetFold * 0.1f;

        svf_f = 0.80f; // 80% Open
        svf_q = 0.15f; // 15% Resonance
      }

      // Propagate to Voices
      float currentRelease = (currentWaveform == WAVE_SINE) ? 2.75f : 1.25f;
      for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].setWaveform(currentWaveform);
        voices[i].setPulseWidth(globalPulseWidth);
        voices[i].releaseTime = currentRelease;
      }
    } // End Skip Update

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
    // Fill buffer again before expensive touch logic
#ifndef ENABLE_BLUETOOTH
    fillAudioBuffer();
#endif

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

    // Check Wave Button (Top Right)
    // Rect: x=SCREEN_WIDTH-50, y=5, w=40, h=30
    if (tx > SCREEN_WIDTH - 60 && ty < 50) {
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
    if (tx < 60 && ty < 50) {
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
}

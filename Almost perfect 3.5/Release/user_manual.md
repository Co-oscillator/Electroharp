# Electroharp v1.1 User Manual (3.5" Build)

Welcome to the Electroharp 3.5, a high-fidelity touch-controlled synthesizer designed for the ESP32 CYD-3.5 board. 

---

## 1. Boot Screen
![Boot Screen](file:///Users/danielmiller/.gemini/antigravity/brain/6b26cdeb-b0fa-4038-9c68-b1db8f56ddd4/electroharp_3_5_boot_v1_1_accurate_png_1766337853889.png)

Upon powering on, you are presented with the output selection menu.
- **BLUETOOTH**: Scan for and connect to a Bluetooth A2DP receiver (speaker/headphones). 
- **SPEAKER**: Enable the onboard speaker and internal DAC.
- **CONFIG**: Enter the system settings menu (Touch Calibration, Functional Test).
- **Calibration**: Hold the **CONFIG** button for 3 seconds to enter the touch screen calibration routine.

---

## 2. Player Screen
![Player Screen](file:///Users/danielmiller/.gemini/antigravity/brain/6b26cdeb-b0fa-4038-9c68-b1db8f56ddd4/electroharp_3_5_player_accurate_png_1766337879318.png)

The main performance interface is divided into four functional rows:

### Top Bar (Controls)
- **Dly**: Cycles through Delay times (Off, 300ms, 600ms, 900ms, 1200ms).
- **Drv / Trm / LFO**: Toggles the Overdrive, Tremolo, and LFO modulation effects.
- **Arp**: 
    - **Tap**: Cycles Arp patterns (Up, Down, U/D, Walk, Rnd, Sparkle).
    - **Long Press**: Toggles **Arp Latch**. When latched, a single tap will un-latch the current pattern.
- **Wave**: 
    - **Tap**: Cycles Waveforms (Saw, Square, Sine, Triangle).
    - **Long Press**: Enters the **Editor Screen**.

### Strings Area
- **Interaction**: Swipe your finger horizontally across the vertical strings to strum.
- **Visuals**: Primary notes are Pale Cream; accidentals are Sky Blue. Root notes (C) are highlighted with circles and labels.
- **Anti-Aliasing**: High-pitched notes automatically "soften" toward smoother waveforms to maintain audio quality.

### Volume & Transpose
- **Volume Slider**: 75% width bar for master gain control.
- **Transpose**: Tapping cycles the musical key from A-G.

### Chord Selection
- **Bank Select**: Tapping a chord button toggles between its variations (Major, Minor, 7th, etc.). The button color indicates the current bank.

---

## 3. Editor Screen
![Editor Screen](file:///Users/danielmiller/.gemini/antigravity/brain/6b26cdeb-b0fa-4038-9c68-b1db8f56ddd4/electroharp_3_5_editor_accurate_png_1766337933612.png)

Deep-dive into the synthesis engine parameters:
- **LFO Tgt / Type**: Assign the LFO to modulate specific parameters (Cutoff, Fold, Pitch).
- **Range**: Adjust the harp span from 1 to 8 octaves.
- **Synth Engine**: Fine-tune Cutoff, Resonance, Drive, and ADSR (Attack/Release) envelopes.
- **EXIT**: Tap the piano icon to return to the Player Screen.

---

## Technical Notes
- **Sample Rate**: 22050Hz (Speaker) / 44100Hz (Bluetooth).
- **Poliphony**: 8 Voices with priority stealing for latched arps.
- **Audio Config**: The advanced audio routing menu is currently a work-in-progress.

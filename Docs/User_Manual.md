# Electroharp Pocket User Manual
**(Unified for 3.5" and 2.8" Versions)**

## 1. System & Connectivity

### 1. Startup & Audio Selection
When you power on the device, you will be greeted by the **Boot Selection Screen**.
This split-screen interface allows you to choose your audio output mode:

- **SPEAKER (Right - Green)**: Selects the internal DAC for wired speaker/headphone output.
- **BLUETOOTH (Left - Blue)**: Initializes the Bluetooth logic to scan for external speakers.
- **CONFIG (Button)**: Enters the System Configuration Menu (LEDs, Audio, Calibration).

> **[i] Version Note:** 
> - **3.5" Model**: The CONFIG button is located on the **middle right** (grey button).
> - **2.8" Model**: The CONFIG button is located at the **bottom center** (grey button).

> **[!] Troubleshooting Tip:** If touch inputs seem misaligned or unresponsive (preventing you from selecting options), **Press and Hold** anywhere on this Boot Screen for **3 seconds**. This will force the device into **Touch Calibration Mode**.

### 2. Bluetooth Connection
If you selected **Bluetooth**, the device will scan for available A2DP sinks (Speakers/Headphones).
The search results will be displayed as colored tiles.
- **Tap a tile** to connect. It will flash while connecting.
- **Refresh** (Grey Tile) to restart the scan.

---

## 2. Playing (Performance Mode)

The main screen is the **Player Interface**, designed for live performance.

### Top Bar Controls
The top bar contains toggle buttons for global effects and settings.

| Button | Label | Function | Active Color |
| :--- | :--- | :--- | :--- |
| **1. Delay** | Delay / Time | Toggles Delay ON/OFF and cycles time (300ms, 600ms, 900ms, 1200ms). | Multi |
| **2. Drive** | Drive | Toggles Overdrive effect. Warms up the sound. | Red |
| **3. Trem** | Trem | Toggles Tremolo (Amplitude Modulation). | Orange |
| **4. LFO** | LFO | Toggles Low Frequency Oscillator modulation. | Magenta |
| **5. Wave** | Icon | Toggles Waveform: Saw (Red), Square (Green), Sine (Blue), Tri (Magenta). **Long Press (250ms)** for Editor. | Multi |

### Strings (Main Area)
- **Strumming**: The main area features a vertical **Piano Keyboard Pattern** (Strings). Touch and drag to play.
- **Visuals**: Keys light up when played. C notes are marked.
> **[i] Version Note:** The 3.5" model displays **49 Strings** (4 Octaves), while the 2.8" model displays **37 Strings** (3 Octaves).

### Volume Slider (Lower Middle)
- **Location**: A full-width cyan bar located below the strings.
- **Action**: Slide left/right to adjust Master Volume.

### Control Row (Bottom)
This row contains Octave Shift and Chord Selection buttons.

**Octave Control (Left 2 Buttons)**
- **Down/Up Arrows**: Shift the playable range down or up by 1 octave.

**Chord Bank (Right 6 Buttons)**
Selects the active chord type.
- **Cycling**: Each button has 5 Banks (variants). Tap repeatedly to cycle:
    1.  **Triad** (Default)
    2.  **7th**
    3.  **Variation 1** (Complex/Inverted)
    4.  **Variation 2** (Extended 9th)
    5.  **Variation 3** (Extended 13th)

### Arpeggiator (Top Bar)
- **Modes**: Tap "Arp" to cycle: Off -> Up -> Down -> Up/Down -> Walk -> Random -> Off.
- **Latch**: **Long Press** "Arp" to Latch (Yellow). The pattern plays automatically (Drone mode).
    - **Random Mode**: Cycling to Random generates a new sequence every time.
- **Manual Play**: When Unlatched, strumming plays the arpeggio pattern relative to the strummed note.

---

## 3. Editor (Sound Design Mode)

To enter, **Long Press** the **Waveform Button** (Top Right) for **250ms**.
To exit, press the **Piano Icon** (Top Left).

### Controls
- **Piano Icon**: Tap to Exit. **Long Press** to edit the Next Waveform (Saw -> Square -> Sine -> Tri have separate settings).
- **Sliders**: Adjust LFO Rate/Depth, Filter Cutoff/Resonance, Envelope Attack/Release, Drive, Fold, and Tremolo Rate.
- **Targets**: Select Modulation Targets for LFO and LDR (Light Sensor).

---

## 4. Configuration & LEDs

### Config Menu
Accessed from the **Boot Screen**.
- **LED SETUP**: Configure LED Hardware.
    - **Swap**: Change RGB ordering (RGB, GRB, etc).
    - **Invert**: Toggle Active-Low/Active-High.
    - **Enable**: Master Toggle for LEDs.
- **AUDIO CONFIG**: Test Audio Hardware (Tone Generator).
- **CALIBRATION**: Recalibrate Touchscreen.

### Runtime LED Toggle
In **Player Mode**, **Long Press** the **DELAY** button (Top Left) for **2 seconds** to quickly toggle LEDs on/off without rebooting.
*Note: Runtime changes are not saved permanently. Use the Config Menu to save.*

---

## 5. Version Differences Summary

| Feature | 3.5" Model | 2.8" Model |
| :--- | :--- | :--- |
| **Screen Size** | 3.5 Inch (480x320) | 2.8 Inch (320x240) |
| **String Count** | 49 Strings (4 Octaves) | 37 Strings (3 Octaves) |
| **Config Button** | Middle Right (Boot Screen) | Bottom Center (Boot Screen) |
| **Audio Hardware** | MAX98357A / PCM5102 | MAX98357A / PCM5102 |

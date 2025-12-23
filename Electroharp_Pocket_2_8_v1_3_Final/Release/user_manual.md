# CYD Autoharp User Manual

## 1. System & Connectivity

### 1. Startup & Audio Selection
When you power on the cyd_autoharp, you will be greeted by the **Boot Selection Screen**.
This split-screen interface allows you to choose your audio output mode:

![Boot Screen Mockup](/Users/danielmiller/.gemini/antigravity/brain/c8099595-ff23-4627-87c4-4c5153529d9a/boot_screen_mockup_1766040070427.png)
*(Note: Visuals are simulated and may slightly differ from firmware)*

- **SPEAKER (Right - Green)**: Selects the internal DAC for wired speaker/headphone output.
- **BLUETOOTH (Left - Blue)**: Initializes the Bluetooth logic to scan for external speakers.
- **CONFIG (Bottom - Grey)**: Enters the System Configuration Menu (LEDs, Audio, Calibration).

> **[i] Troubleshooting Tip:** If touch inputs seem misaligned, **Press and Hold** anywhere on this Boot Screen for **3 seconds** to force **Touch Calibration Mode**.

### 2. Bluetooth Connection
If you selected **Bluetooth**, the device will scan for available A2DP sinks (Speakers/Headphones).
The search results will be displayed in a grid:

![Bluetooth Selection Mockup](/Users/danielmiller/.gemini/antigravity/brain/c8099595-ff23-4627-87c4-4c5153529d9a/bluetooth_select_mockup_1766040098757.png)
*(Note: Visuals are simulated and may slightly differ from firmware)*

- **Device Tiles**: Tap a colored tile to connect to that device. The tile will flash while connecting.
- **Refresh (Grey Tile)**: Tap to clear the list and restart the scan if your device isn't found.
- **Status**: The serial console (if monitoring) provides connection status updates. On the device, successful connection allows audio playback.

---

## 2. Playing (Performance Mode)

The main screen is the **Player Interface**, designed for live performance.

![Player Interface](/Users/danielmiller/.gemini/antigravity/brain/c8099595-ff23-4627-87c4-4c5153529d9a/player_screen_interface_1765984404514.png)
*(Note: Layout in image may vary slightly from latest firmware)*

### Top Bar Controls
The top bar contains toggle buttons for global effects and settings.

| Button | Label | Function | Active Color |
| :--- | :--- | :--- | :--- |
| **1. Delay** | Delay / Time | Toggles Delay ON/OFF and cycles time. Options: **300ms** (Green), **600ms** (Cyan), **900ms** (Yellow), **1200ms** (Magenta). | Multi |
| **2. Drive** | Drive | Toggles Overdrive effect. Warms up the sound. | Red |
| **3. Trem** | Trem | Toggles Tremolo (Amplitude Modulation). | Orange |
| **4. LFO** | LFO | Toggles Low Frequency Oscillator modulation. | Magenta |
| **5. Wave** | Icon | Toggles Waveform: Saw (Red), Square (Green), Sine (Blue), Tri (Magenta). **Long Press (250ms)**: Enter Editor Mode. | Multi |

### Strings (Main Area)
- **Strumming**: The main area features a vertical **Piano Keyboard Pattern** (Strings). Touch and drag across the keys/strings to play notes.
- **Visuals**: Keys light up when played. C notes are marked.

### Volume Slider (Lower Middle)
- **Location**: A full-width bar located just **below the strings** and **above the bottom buttons**.
- **Action**: Slide left/right to adjust Master Volume.
- **Visual**: Cyan bar with "Volume" text.

### Control Row (Bottom)
A single row of **8 Buttons** at the bottom of the screen.

**Octave Control (Left 2 Buttons)** (Arrow Icons)
- **Down**: Shift down 1 octave.
- **Up**: Shift up 1 octave.
- **Active State**: Buttons glow **Cyan** when an octave shift is active.

**Chord Bank (Right 6 Buttons)**
Selects the active chord type for the strumming area.
- **Logic**: When a chord is active, its button lights up with a specific color. All others show "Off". If no chord is active, buttons show their default labels.
- **Banks**: Each chord button can cycle through **5 Banks** (variants) by tapping:
    1.  **Bank 0**: Default (Triad)
    2.  **Bank 1**: 7th / Extension
    3.  **Bank 2**: Variation 1 (Complex/Inverted)
    4.  **Bank 3**: Variation 2 (Extended - e.g. 9th)
    5.  **Bank 4**: Variation 3 (Extended - e.g. 13th)

### Arpeggiator
The Arpeggiator plays the notes of the active chord in a sequence.
- **Button**: The "Arp" button is located in the top bar (2nd from right).
- **Modes**: Tap the button to cycle through modes:
    - **Off**: Arpeggiator disabled.
    - **Up**: Notes play from lowest to highest.
    - **Down**: Notes play from highest to lowest.
    - **Up/Down**: Notes play up then down.
    - **Walk**: A staggered walking pattern.
    - **Random**: Notes play in a random order.
- **Latch**: **Long Press** the Arp button to toggle Latch mode. When Latched (Yellow button), the arpeggio plays automatically as a "drone" layer. You can strum freely on top of the latched sequence.
- **Random Gen**: Cycling to the **Random** mode generates a new random sequence each time. To get a new pattern, simply tap through the modes back to Random.
- **Manual Play**: When Unlatched, strumming the strings advances the arpeggio. The arpeggio will play in the **octave** where you strum, allowing you to transpose the pattern up and down the keyboard dynamically.

---


## 3. Editor (Sound Design Mode)

To enter the Editor, **Long Press** the **Waveform Button** (Top Right) for **250ms**.
To exit, press the **Piano Icon** (Top Left) or **Long Press** the Piano Icon to **cycle to the next waveform's editor**.

![Editor Interface](/Users/danielmiller/.gemini/antigravity/brain/c8099595-ff23-4627-87c4-4c5153529d9a/editor_screen_interface_1765984699381.png)
*(Note: Layout in image may vary slightly from latest firmware)*

### Top Row Controls
- **Piano Icon**: 
    - **Tap**: Exit to Play Mode.
    - **Long Press (250ms)**: Cycle to the Next Waveform (Edit specific parameters for Saw -> Square -> Sine -> Tri).
- **LFO Target**: Selects what the LFO modulates.
    - *Options*: `NONE`, `FOLD` (Wavefold), `FILTER` (Cutoff), `RES` (Resonance), `PITCH` (Vibrato), `ATK` (Attack), `REL` (Release), `LFOD` (LFO Depth).
- **LFO Type**: Selects LFO shape.
    - *Options*: `SINE`, `SQUARE`, `RAMP`, `NOISE`.
- **LDR Target**: Selects what the Light Sensor (LDR) modulates.
    - *Options*: Same as LFO Target list (including `LFOD`).

### Parameter Sliders
9 Sliders allow fine-tuning of the synthesizer engine.

**Row 1: Modulation**
- **LFO Hz**: Speed of the LFO (0.08 Hz - 16.0 Hz). Curve is non-linear for fine low-end control.
- **LFO Depth**: Intensity of the LFO modulation.
- **Drive**: Amount of saturation/distortion.

**Row 2: Timbre**
- **Filter**: Low-pass filter cutoff frequency.
- **Resonance**: Filter resonance (Q factor).
- **Fold**: Wavefolder amount (adds harmonics).

**Row 3: Envelope**
- **Attack**: Time for note to fade in.
- **Release**: Time for note to fade out after string release.
- **Volume Slider**: Touch and drag the Cyan bar at the bottom left to adjust Main Output Volume.
- **Transpose Button**: Tap the Magenta button at the bottom right to cycle the Key (Root Note).
    - **Visuals**: The button displays the current Root Note (e.g. "Transpose F#"). The keyboard pattern on the strings shifts effectively to match the new key (String 0 is always the Root).
    - **Audio**: All chords and arpeggios are pitch-shifted to the new key.
- **Arpeggiator**: Added persistence during Editor Mode (Drone continues). Clarified Manual Play behavior (Monophonic).
- **Trem Hz**: Speed of the Tremolo effect.

### Interaction Tips
- **Toggle**: Tap the top labels (e.g., LFO Tgt) to cycle options.
- **Hold Feedback**: Both the **Waveform Button** (Player) and **Piano Button** (Editor) will glow **Yellow -> Red** when held, indicating the mode/waveform switch is about to trigger.

# Features & Refinements - V4

## Overview
This release addresses logic issues with LDR targeting and fine-tunes the LFO controls for extended range.

### Logic Improvements
- **LFO Depth Targeting (Fixed)**:
    - Fixed a bug where LDR modulation of LFO Depth was not being applied to the audio engine effectively.
    - Implemented a thread-safe global depth parameter (`globalLfoDepth`) to ensure the **LDR -> LFOD** modulation works correctly across the UI, Logic, and Audio threads.
- **LFO Rate Control**:
    - **Extended Range**: Minimum frequency lowered further to **0.08 Hz** and maximum raised to **16.0 Hz** (was 10.0 Hz).
    - **Non-Linear Scaling**: Maintained quadratic scaling to ensure the new sub-0.1Hz range is easily controllable via the slider.

### Files
- **Firmware**: `Release/CYD_Autoharp_Firmware.bin` (Updated V4).
- **Source Backup**: `cyd_autoharp_final_v4`.

## Arpeggiator Implementation (Dec 17)
Implemented a full-featured Arpeggiator for the Autoharp.

### Changes
- **UI**: Resized top bar to 6 buttons. Added `Arp` button with color-coded modes.
- **Logic**: Added `ArpMode` enum (Off, Up, Down, Up/Down, Walk, Random).
- **Interaction**:
    - **Tap**: Cycle Modes (If Random selected, reshuffles pattern).
    - **Long Press**: Toggle Latch (Yellow). Plays automatically using LFO rate.
    - **Interaction**: Latched Arp plays independently of strumming (drone-like). Unlatched Strumming releases only its own notes.
    - **Strum**: Advances arpeggiator step when unlatched with "Octave Smart" transposition.
- **Engine**: 
    - **Latch Envelope**: Custom ADSR for Latched notes (Slower, drone-like sustain).
    - **Speed**: Latched playback speed is 20% of LFO Hz (Slow Drone).
    - **Logic**: Integrated `fireArp()` with robust voice management (Stealing & Zone Release checks). Fixed a critical input bug where `waitForArpRelease` could latch indefinitely if input handling was interrupted (e.g. Mode Switch). It now resets reliably at the top of the loop. Fixed Latch/Release timing to prevent unintended unlatching on release.

### Verification
- **Compilation**: Success.
- **Manual**: Updated with new Arpeggiator section.

## Transpose Implementation (Dec 18)
Added global Transpose functionality to change the Key of the instrument.

### Changes
- **UI**: 
    - Resized Volume Slider (Width 240px).
    - Added **Transpose Button** (Width 80px) at bottom right.
    - Visuals: Button displays Root Note (e.g. "Transpose F#"). Keyboard pattern on strings visually shifts to match the new key (String 0 is always Root).
- **Logic**:
    - Tap Transpose Button to cycle Root Note (0-11 / C-B).
- **Audio Engine**:
    - Applied Pitch Shift (2^(root/12)) to all voice triggers (Manual, Arp, Strum).

## Drone Octave Refinement (Dec 18)
- Added `latchedOctaveShift` global variable.
- Updated Arp latch logic in `loop()` to capture `octaveShift` upon engagement.
- Modified `fireArp()` to use the locked `latchedOctaveShift` for latched note triggers, allowing independent keyboard octave changes while the drone is running.


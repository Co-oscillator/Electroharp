# Walkthrough: Audio & UI Refinements

We have successfully resolved the "High Pitched / Garbled" audio issue and enhanced the boot experience with a new splash animation.

## Key Changes

### 1. Audio Pitch & Quality Fixes
- **Corrected Pitch Multiplier**: Fixed a logic error where `pitchMod` was initialized to `0.0`, rendering oscillators silent or garbled. It is now correctly initialized to `1.0`.
- **Sample Rate Safety**: Added a robust safety fallback in `SynthVoice.cpp` to ensure `activeSampleRate` never defaults to zero, preventing infinite pitch increments.
- **Restored Processing Chain**: Re-enabled the Chamberlin SVF Filter and the full FX chain (Drive, Delay, Tremolo) now that the base oscillator pitch is stable.
- **Optimized ISR**: Implemented direct register DAC writes (`SET_PERI_REG_BITS`) for minimum jitter and cleaner output.

### 2. Animated Splash Screen
- **Animation Loop**: Converted the static splash screen into a 3-frame "lightning" animation with 300ms delays, creating a more dynamic startup effect as requested.

### 3. Fine-Tuning & Refinements
- **Speaker Q-Compensation (+50%)**: Increased damping in the speaker profile to stop filter ringing, specifically tuned for the 3.5" hardware.
- **Root-Tracking String Labels**: String labels now correctly track the `rootNote`. String 0, 12, etc., will always show the Root Note name (e.g., "F#" if transposed to F#) and maintain correct white/black key coloring.
- **UI Boundary Fixes**: Fixed the logic in `updateStringVisuals` to prevent malformed labels from appearing when the string range is changed or transposed.
- **Latched Arp Octave Boost**: Latched arpeggios now play +2 octaves higher than the manual strum, creating a more distinct "shimmer" effect.
- **Sine Normalization**: Sine wave pitch is now normalized to match all other waveforms.

### 4. Polyphony Expansion
- **MAX_VOICES increased to 18**: The synthesizer now supports up to 18 physical voices, providing a richer, more complex sound during dense playing.
- **Granular CPU Governor**: Implemented a four-stage dynamic governor (18, 14, 10, 6 voices) that adjusts polyphony in real-time based on CPU load to maintain audio stability without compromising quality.

## Backup Created
A full snapshot of the stable v1.3 has been created at:
`.../Triple Pitch Delay/Electroharp_Pocket_3_5_v1_3_Final`

### 5. Augment 2.8 Fixes
- **Bluetooth Stability**: Implemented the `isBluetoothActive` safety flag to prevent internal crashes when `a2dp_source.end()` is called on an uninitialized stream. This resolves the reported pairing crashes.
- **Root-Tracking String Labels**: Ported the improved 3.5" labeling logic to Version 2.8. String labels now move with transposition and always show the correct Root Note name.
- **Animated Splash Screen**: Implemented the 3-frame "lightning" animation (3x 300ms) at boot to match the 3.5" experience.
- **Sine Normalization**: Normalized Sine wave pitch to match other waveforms.
- **Visual Feedback**: Changed the string highlight color from Orange to **Bright Red** in both the 2.8 and 3.5 versions for better high-contrast visibility.
- **UI Consistency**: Updated the 3.5 model's Volume Bar to **Cyan** to match the aesthetic of the 2.8 version.

### 6. Electroharp 2.8 v1.3 Graduation
- **Stabilization**: Resolved Arp Latch crash by implementing Priority-Based Voice Stealing. The system now gracefully handles high note density without cutting off active notes or crashing.
- **UI Redesign**: Simplified bottom row to 6 buttons (Oct-, 4x Chord Banks, Oct+).
- **Consolidated Chords**: Re-grouped chords into Major (Maj/Sus), Minor (Min/Dim), Extensions (7/9/11/13), and Mixed Technical banks.
- **Touch Refinement**: Implemented 10px dead zones (5px buffers) between all interactive zones (Top Bar, Strings, Sliders, Bottom Bar) to prevent accidental triggers during performance.
- **Version Updates**: Graduation from "Augment" to formal **Electroharp 2.8 v1.3** branding.

### 7. Final v1.3 Refinements (Both Versions)
- **String Range Feedback**: Strings now dim at both the low and high ends if their frequency is outside the playable 20Hz - 4800Hz range or buffer limits.
- **Quadratic Volume Curve**: Re-mapped the volume slider to a `pos * pos` curve, providing 4x more precision at low volumes for professional mixing.
- **Octave Navigation**: Added white numeric shift labels (+1, -2, etc.) to the outer corners of the octave buttons for instant orientation.
- **Touch Shielding**: Reduced dead zones to a sharp 3px border (6px total buffer) and implemented horizontal dead zones to isolate buttons from each other.
- **Reliable Editor Launch**: Fine-tuned the Waveform button long-press (600ms) to ensure consistent entry into the Editor across all hardware.
- **Note Range Shift**: Shifted the base playable range to **C1 - C9** (97 notes total). Both models are now perfectly synchronized to this range, pitched exactly one octave down from the original release.
- **Precision Touch Calibration**: Moved the calibration targets to the absolute screen corners and added crosshair guides. This ensures that the touch mapping accounts for the full physical display area, eliminating edge misalignment.
- **Enhanced Filter Damping**: Increased Resonance (Q) compensation by 30% across all waveforms and output profiles. This significantly reduces high-frequency squeaking and ringing, especially on the built-in speaker.
- **Improved Arp Latch Flow**: Strumming strings now correctly layers over Latched Arp voices instead of cutting them off, allowing for richer polyphonic performances.

## Verification Results

### Bluetooth Testing (Version 2.8)
- **Pairing Stability**: Verified that repetitive pairing and mode switching no longer cause crashes or reboots.
- **Refresh Logic**: Confirmed the "Refresh Scan" button in the BT menu now safely resets the radio.

### UI Testing (Version 2.8)
- **Label Tracking**: Confirmed labels correctly track the root note across all transpositions.
- **Boot Animation**: Verified the 3-frame lightning animation displays correctly on the 2.8" screen.

> [!IMPORTANT]
> The diagnostic `Serial.printf` logging from `triggerNote` and `setup` has been removed to ensure maximum performance during gameplay.

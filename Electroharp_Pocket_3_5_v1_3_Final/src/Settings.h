#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <Preferences.h>

struct CalibrationData {
  uint16_t minX;
  uint16_t maxX;
  uint16_t minY;
  uint16_t maxY;
  bool swapXY;
  bool isCalibrated; // New Flag
};

class Settings {
public:
  Settings();

  void begin();
  void load();
  void save();

  // Reset to defaults
  void reset();

  // Data
  CalibrationData touch;
  int defaultAudioMode;  // 0=BootMenu, 1=Speaker, 2=BT
  int audioProfileIndex; // 0=Default, 1+ = Custom Pin Combos

private:
  Preferences prefs;
  const char *NAMESPACE = "autoharp";
};

extern Settings settings;

#endif

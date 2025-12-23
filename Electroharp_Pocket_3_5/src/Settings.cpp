#include "Settings.h"
#include "Config.h" // For Defaults

Settings settings;

Settings::Settings() {
// Safe defaults (can be overwritten by load)
// Check if Config.h defines TS_MINX etc. If not, use safe defaults.
#ifdef TS_MINX
  touch.minX = TS_MINX;
#else
  touch.minX = 200;
#endif

#ifdef TS_MAXX
  touch.maxX = TS_MAXX;
#else
  touch.maxX = 3800;
#endif

#ifdef TS_MINY
  touch.minY = TS_MINY;
#else
  touch.minY = 200;
#endif

#ifdef TS_MAXY
  touch.maxY = TS_MAXY;
#else
  touch.maxY = 3800;
#endif

  touch.swapXY = true;
  touch.isCalibrated = false; // Default
  defaultAudioMode = 0;
  audioProfileIndex = 0;
}

void Settings::begin() {
  prefs.begin(NAMESPACE, false); // Read/Write
}

void Settings::load() {
  // Load or use hardcoded defaults if missing
  touch.minX = prefs.getUShort("minX", touch.minX);
  touch.maxX = prefs.getUShort("maxX", touch.maxX);
  touch.minY = prefs.getUShort("minY", touch.minY);
  touch.maxY = prefs.getUShort("maxY", touch.maxY);
  touch.maxY = prefs.getUShort("maxY", touch.maxY);
  touch.swapXY = prefs.getBool("swapXY", true);
  touch.isCalibrated = prefs.getBool("isCal", false);

  defaultAudioMode = prefs.getInt("audioMode", 0);
  audioProfileIndex = prefs.getInt("audioProf", 0);

  Serial.println("Settings Loaded from NVS");
  Serial.printf("Touch: X(%d-%d) Y(%d-%d) Swap:%d\n", touch.minX, touch.maxX,
                touch.minY, touch.maxY, touch.swapXY);
}

void Settings::save() {
  prefs.putUShort("minX", touch.minX);
  prefs.putUShort("maxX", touch.maxX);
  prefs.putUShort("minY", touch.minY);
  prefs.putUShort("maxY", touch.maxY);
  prefs.putUShort("maxY", touch.maxY);
  prefs.putBool("swapXY", touch.swapXY);
  prefs.putBool("isCal", touch.isCalibrated);
  prefs.putInt("audioMode", defaultAudioMode);
  prefs.putInt("audioProf", audioProfileIndex);
  Serial.println("Settings Saved to NVS");
}

void Settings::reset() {
  prefs.clear();
// Restore RAM settings to defaults
#ifdef TS_MINX
  touch.minX = TS_MINX;
#else
  touch.minX = 200;
#endif

#ifdef TS_MAXX
  touch.maxX = TS_MAXX;
#else
  touch.maxX = 3800;
#endif

#ifdef TS_MINY
  touch.minY = TS_MINY;
#else
  touch.minY = 200;
#endif

#ifdef TS_MAXY
  touch.maxY = TS_MAXY;
#else
  touch.maxY = 3800;
#endif

  touch.swapXY = true;
  touch.isCalibrated = false;
  defaultAudioMode = 0;
  save(); // Write defaults back
  Serial.println("Settings Reset to Defaults");
}

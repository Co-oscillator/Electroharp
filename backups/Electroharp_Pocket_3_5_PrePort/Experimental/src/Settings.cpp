#include "Settings.h"
#include "Config.h" // For Defaults

Settings settings;

Settings::Settings() {
  // Safe defaults (can be overwritten by load)
  touch.minX = TS_MINX;
  touch.maxX = TS_MAXX;
  touch.minY = TS_MINY;
  touch.maxY = TS_MAXY;
  touch.swapXY = true; // Default for this screen
  defaultAudioMode = 0;
  audioProfileIndex = 0;
  ledExperimental = false; // Walled off by default
  isInitialized = false;
}

void Settings::begin() {
  prefs.begin(NAMESPACE, false); // Read/Write
}

void Settings::load() {
  // Load or use hardcoded defaults if missing
  touch.minX = prefs.getUShort("minX", TS_MINX);
  touch.maxX = prefs.getUShort("maxX", TS_MAXX);
  touch.minY = prefs.getUShort("minY", TS_MINY);
  touch.maxY = prefs.getUShort("maxY", TS_MAXY);
  touch.swapXY = prefs.getBool("swapXY", true);

  defaultAudioMode = prefs.getInt("audioMode", 0);
  audioProfileIndex = prefs.getInt("audioProf", 0);
  ledExperimental = prefs.getBool("ledExp", false);
  ldrEnabled = prefs.getBool("ldrEn", true);
  isInitialized = prefs.getBool("isInit", false);

  Serial.println("--- NVS LOAD ---");
  Serial.printf("Namespace: %s, isInit: %d\n", NAMESPACE, isInitialized);
  Serial.printf("Touch: X(%d-%d) Y(%d-%d) Swap:%d\n", touch.minX, touch.maxX,
                touch.minY, touch.maxY, touch.swapXY);
}

void Settings::save() {
  Serial.println("--- NVS SAVE ---");
  Serial.printf("Namespace: %s, Setting isInit to: %d\n", NAMESPACE,
                isInitialized);
  prefs.putUShort("minX", touch.minX);
  prefs.putUShort("maxX", touch.maxX);
  prefs.putUShort("minY", touch.minY);
  prefs.putUShort("maxY", touch.maxY);
  prefs.putBool("swapXY", touch.swapXY);
  prefs.putInt("audioMode", defaultAudioMode);
  prefs.putInt("audioProf", audioProfileIndex);
  prefs.putBool("ledExp", ledExperimental);
  prefs.putBool("ldrEn", ldrEnabled);
  prefs.putBool("isInit", isInitialized);
  Serial.println("Settings Saved to NVS");
}

void Settings::reset() {
  prefs.clear();
  // Restore RAM settings to defaults
  touch.minX = TS_MINX;
  touch.maxX = TS_MAXX;
  touch.minY = TS_MINY;
  touch.maxY = TS_MAXY;
  touch.swapXY = true;
  defaultAudioMode = 0;
  audioProfileIndex = 0;
  ledExperimental = false;
  ldrEnabled = true;
  isInitialized = false;
  save(); // Write defaults back
  Serial.println("Settings Reset to Defaults");
}


// --- PIN DIAGNOSTIC MODE ---
int diagPin = 26;
void runPinDiagnostic() {
  static uint32_t lastTone = 0;
  static bool state = false;
  static int checkPin = 26;

  // Safety: Ensure Enable Pins are HIGH
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  pinMode(22, OUTPUT);
  digitalWrite(22, HIGH);
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  pinMode(32, OUTPUT);
  digitalWrite(32, HIGH);
  pinMode(33, OUTPUT);
  digitalWrite(33, HIGH);

  // Draw Only on Change
  static int lastDrawnPin = -1;
  if (diagPin != lastDrawnPin) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(4);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("AUDIO TEST", 240, 50);
    tft.drawString("PIN: " + String(diagPin), 240, 160);
    tft.setTextSize(2);
    tft.drawString("< PREV       NEXT >", 240, 240);
    tft.drawString("Touch to Scan", 240, 280);
    lastDrawnPin = diagPin;

    // Force pin to output
    pinMode(diagPin, OUTPUT);
  }

  // Generate 440Hz Square Wave (Blocking-ish)
  if (micros() - lastTone > 1136) {
    state = !state;
    digitalWrite(diagPin, state);
    lastTone = micros();
  }

  // Input (Polled)
  if (ts.touched()) {
    TS_Point p = ts.getCalibratedPoint();
    static uint32_t lastIn = 0;
    if (millis() - lastIn > 250) {
      int dir = 0;
      if (p.x < 120)
        dir = -1;
      if (p.x > 360)
        dir = 1;

      if (dir != 0) {
        // Determine next pin
        do {
          diagPin += dir;
          // Wrap
          if (diagPin > 33)
            diagPin = 4;
          if (diagPin < 4)
            diagPin = 33;
          // Skip Unsafe or Input Only
          // Safe: 4, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27,
          // 32, 33
        } while (!((diagPin >= 4 && diagPin <= 4) ||
                   (diagPin >= 12 && diagPin <= 19) ||
                   (diagPin >= 21 && diagPin <= 23) ||
                   (diagPin >= 25 && diagPin <= 27) ||
                   (diagPin >= 32 && diagPin <= 33)));

        // Reset State
        lastIn = millis();
      }
    }
  }
}

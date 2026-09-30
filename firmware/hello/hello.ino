// Step 0: say hello over USB. Prints on both the native-USB port (Serial) and the UART port (Serial0).
static void both(const String &s) { Serial.println(s); Serial0.println(s); }

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(1500);
}

void loop() {
  static uint32_t n = 0;
  both("hello from the ESP32-S3 #" + String(n++));
  both("  chip " + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + ", " + String(ESP.getChipCores()) + " cores, " + String(ESP.getCpuFreqMHz()) + " MHz");
  both("  flash " + String(ESP.getFlashChipSize() / 1048576) + " MB, psram " + String(ESP.getPsramSize() / 1048576) + " MB, free heap " + String(ESP.getFreeHeap() / 1024) + " KB");
  both("  mac " + String(ESP.getEfuseMac(), HEX));
  delay(2000);
}

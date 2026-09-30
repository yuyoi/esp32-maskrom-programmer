// SST39SF040 WiFi programmer for ESP32-S3 (write-only, no readback).
// The ESP makes its own WiFi network, you upload a 512 KB card .bin on the web page, it keeps up to ~17 of them
// in flash and burns the one you pick. Use the board's UART USB port (the native USB pins 19/20 are wired to the SST).
//
// Wiring: see the pin tables below. SST CE# -> GND, OE# -> +5 V, VDD -> +5 V, 100 nF across VDD/VSS.
// Only power the SST from the ESP while the card is OUT of the U-110.
#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "esp_rom_crc.h"
#include "soc/gpio_reg.h"
#include "soc/usb_serial_jtag_reg.h"

// ---- change these if you rewire ----------------------------------------------------------------------
static const char *AP_SSID = "SST-PROG";
static const char *AP_PASS = "u110cards";                 // 8+ chars
static const uint8_t ADDR_PIN[19] = {42, 41, 40, 39, 38, 0, 45, 48, 10, 9, 3, 46, 47, 11, 12, 21, 20, 13, 19};  // A0..A18
static const uint8_t DATA_PIN[8] = {2, 1, 7, 15, 16, 17, 18, 8};                                                  // DQ0..DQ7
static const uint8_t WE_PIN = 14;                                                                                  // WE#
// -------------------------------------------------------------------------------------------------------

static const uint32_t IMAGE_SIZE = 512 * 1024;
static const char *DIR_CARDS = "/cards";

WebServer server(80);

// ---------------------------------------------------------------- bus
static inline void putPins(const uint8_t *pins, int n, uint32_t v) {
  uint32_t setLo = 0, clrLo = 0, setHi = 0, clrHi = 0;
  for (int i = 0; i < n; i++) {
    uint8_t p = pins[i];
    bool one = (v >> i) & 1;
    if (p < 32) { if (one) setLo |= 1UL << p; else clrLo |= 1UL << p; }
    else        { if (one) setHi |= 1UL << (p - 32); else clrHi |= 1UL << (p - 32); }
  }
  REG_WRITE(GPIO_OUT_W1TS_REG, setLo);  REG_WRITE(GPIO_OUT_W1TC_REG, clrLo);
  REG_WRITE(GPIO_OUT1_W1TS_REG, setHi); REG_WRITE(GPIO_OUT1_W1TC_REG, clrHi);
}

// one write cycle: address + data settle, WE# low >= 40 ns (we hold ~1 us), WE# high
static inline void busWrite(uint32_t addr, uint8_t data) {
  putPins(ADDR_PIN, 19, addr);
  putPins(DATA_PIN, 8, data);
  REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << WE_PIN);
  delayMicroseconds(1);
  REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << WE_PIN);
}

static void initBus() {
#if defined(USB_SERIAL_JTAG_CONF0_REG)
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);   // free GPIO19/20 from the USB PHY
#endif
  digitalWrite(WE_PIN, HIGH);                     // WE# high before it becomes an output
  pinMode(WE_PIN, OUTPUT);
  for (int i = 0; i < 19; i++) { digitalWrite(ADDR_PIN[i], LOW); pinMode(ADDR_PIN[i], OUTPUT); }
  for (int i = 0; i < 8; i++)  { digitalWrite(DATA_PIN[i], LOW); pinMode(DATA_PIN[i], OUTPUT); }
}

// SST39SF040 command sequences (datasheet table 4): addresses 5555H / 2AAAH
static void cmdUnlock() { busWrite(0x5555, 0xAA); busWrite(0x2AAA, 0x55); }
static void chipErase() {
  cmdUnlock(); busWrite(0x5555, 0x80);
  cmdUnlock(); busWrite(0x5555, 0x10);
  delay(150);                                     // TSCE max 100 ms
}
static void byteProgram(uint32_t addr, uint8_t d) {
  cmdUnlock(); busWrite(0x5555, 0xA0); busWrite(addr, d);
  delayMicroseconds(25);                          // TBP max 20 us
}

// ---------------------------------------------------------------- radio off while burning (no RF noise / current spikes)
static void radioOff() { WiFi.disconnect(true, false); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); }
static void radioOn() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Preferences p; p.begin("wifi", true); String s = p.getString("ssid", ""), pw = p.getString("pass", ""); p.end();
  if (s.length()) WiFi.begin(s.c_str(), pw.c_str());
  MDNS.end(); if (MDNS.begin("sstprog")) MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------- burn job (runs in its own task)
enum { J_IDLE, J_ERASING, J_WRITING, J_DONE, J_ERROR };
static volatile int jobState = J_IDLE;
static volatile uint32_t jobDone = 0;
static char jobName[64] = "";
static char jobMsg[80] = "";

static void burnTask(void *arg) {
  String path = String(DIR_CARDS) + "/" + jobName;
  File f = FFat.open(path, "r");
  if (!f || f.size() != IMAGE_SIZE) {
    snprintf(jobMsg, sizeof jobMsg, "%s is not a 512 KB image", jobName);
    jobState = J_ERROR; if (f) f.close(); vTaskDelete(NULL);
  }
  jobDone = 0; jobState = J_ERASING;
  vTaskDelay(pdMS_TO_TICKS(400));                 // let the HTTP reply out, then kill the radio
  radioOff();
  chipErase();
  jobState = J_WRITING;
  static uint8_t buf[512];
  uint32_t addr = 0;
  uint32_t t0 = millis();
  while (addr < IMAGE_SIZE) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) if (buf[i] != 0xFF) byteProgram(addr + i, buf[i]);   // erased bytes are already FF
    addr += n; jobDone = addr;
    if ((addr & 0x1FFF) == 0) vTaskDelay(1);      // let the web server breathe
  }
  f.close();
  int st;
  if (addr == IMAGE_SIZE) { snprintf(jobMsg, sizeof jobMsg, "wrote %s in %lu s - verify in the T48", jobName, (unsigned long)((millis() - t0) / 1000)); st = J_DONE; }
  else { snprintf(jobMsg, sizeof jobMsg, "read error at %lu", (unsigned long)addr); st = J_ERROR; }
  radioOn();
  jobState = st;
  Serial.println(jobMsg);
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------- web
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>SST programmer</title><style>
body{font-family:system-ui,sans-serif;background:#12161a;color:#eef1f3;max-width:640px;margin:20px auto;padding:0 14px}
h1{color:#6ec8e8;font-size:1.4em}.card{background:#1d2228;border:1px solid #33393f;border-radius:8px;padding:12px;margin:10px 0}
button,input[type=submit]{background:#3a3f46;color:#eef1f3;border:1px solid #555;border-radius:5px;padding:7px 14px;font-size:1em}
button.go{background:#c8322a;border-color:#c8322a}.row{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0}
#bar{height:14px;background:#0c0f0d;border-radius:7px;overflow:hidden}#fill{height:100%;width:0;background:#94ec44}
small{color:#8a96a0}</style></head><body>
<h1>SST39SF040 programmer</h1>
<div class=card><b>Stored cards</b><div id=list>...</div></div>
<div class=card><b>Upload a card image</b> <small>(512 KB .bin, connector order)</small>
<form method=POST action=/upload enctype=multipart/form-data><div class=row><input type=file name=f accept=.bin required><input type=submit value=Upload></div></form></div>
<div class=card><b>Home WiFi</b> <small id=wf>...</small>
<form method=POST action=/wifi><div class=row><input name=ssid placeholder=network required><input name=pass type=password placeholder=password><input type=submit value=Join></div></form>
<small>Joins your router too, so a PC on the same network can open the page. The SST-PROG network stays on.</small></div>
<div class=card><b>Status</b><div id=st>idle</div><div id=bar><div id=fill></div></div></div>
<small>Only burn with the card OUT of the U-110.</small>
<script>
async function j(u){return (await fetch(u)).json()}
async function refresh(){
  const l=await j('/list');let h='';
  for(const c of l.cards)h+=`<div class=row><span>${c.name} <small>${(c.size/1024)|0} KB</small></span><span><button class=go onclick="burn('${c.name}')">Burn</button> <button onclick="del('${c.name}')">Delete</button></span></div>`;
  document.getElementById('list').innerHTML=h||'<small>none yet</small>';
  document.getElementById('st').textContent=l.free_kb+' KB free';
}
async function burn(n){if(!confirm('Erase the SST and burn '+n+'?'))return;const r=await j('/burn?f='+encodeURIComponent(n));if(r.error)alert(r.error);poll()}
async function del(n){if(!confirm('Delete '+n+'?'))return;await fetch('/del?f='+encodeURIComponent(n));refresh()}
async function poll(){
  let s;
  try{s=await j('/status')}catch(e){document.getElementById('st').textContent='WiFi is off while burning - back in a few seconds...';setTimeout(poll,1500);return}
  const names=['idle','erasing chip...','writing','DONE','ERROR'];
  document.getElementById('st').textContent=names[s.state]+(s.msg?' - '+s.msg:'');
  document.getElementById('fill').style.width=(s.state==1?3:100*s.done/s.total)+'%';
  if(s.state==1||s.state==2)setTimeout(poll,400);else refresh();
}
async function wifi(){
  const w=await j('/wifi');
  document.getElementById('wf').textContent=w.connected?('connected to '+w.ssid+': http://'+w.ip+'/ or http://sstprog.local/'):(w.ssid?('joining '+w.ssid+'...'):'not set');
  if(w.ssid&&!w.connected)setTimeout(wifi,2000);
}
refresh();poll();wifi();
</script></body></html>)HTML";

static String cleanName(String n) {
  int s = max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
  if (s >= 0) n = n.substring(s + 1);
  String o;
  for (char c : n) if (isalnum(c) || c == '.' || c == '-' || c == '_') o += c;
  if (o.length() > 40) o = o.substring(o.length() - 40);
  if (!o.endsWith(".bin")) o += ".bin";
  return o;
}

static File uploadFile;
static bool uploadOk = true;
static String uploadMsg;

static void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadOk = true; uploadMsg = "";
    if (jobState == J_ERASING || jobState == J_WRITING) { uploadOk = false; uploadMsg = "burn in progress"; return; }
    if (FFat.freeBytes() < IMAGE_SIZE + 4096) { uploadOk = false; uploadMsg = "storage full - delete a card first"; return; }
    uploadFile = FFat.open(String(DIR_CARDS) + "/" + cleanName(up.filename), "w");
    if (!uploadFile) { uploadOk = false; uploadMsg = "cannot create file"; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOk && uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) { size_t sz = uploadFile.size(); String p = String(uploadFile.path()); uploadFile.close();
      if (sz != IMAGE_SIZE) { FFat.remove(p); uploadOk = false; uploadMsg = "file must be exactly 512 KB (got " + String(sz) + ")"; } }
  }
}

static String jsonEscape(const String &s) { String o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o; }

// ---------------------------------------------------------------- burn start (shared by the web page and USB)
static bool startBurn(String n, String &err) {
  if (jobState == J_ERASING || jobState == J_WRITING) { err = "busy"; return false; }
  n = cleanName(n);
  if (!FFat.exists(String(DIR_CARDS) + "/" + n)) { err = "no such file"; return false; }
  n.toCharArray(jobName, sizeof jobName); jobMsg[0] = 0; jobDone = 0; jobState = J_ERASING;
  xTaskCreatePinnedToCore(burnTask, "burn", 6144, NULL, 1, NULL, 1);
  return true;
}

// ---------------------------------------------------------------- USB serial protocol (921600 baud, line commands)
//   PING | LIST | DEL name | PUT name size crchex (then raw data in 4 KB blocks, each acked with 'K') | BURN name | STATUS
static void serialPut(const char *name, uint32_t size, uint32_t crcWant) {
  if (jobState == J_ERASING || jobState == J_WRITING) { Serial.println("ERR busy"); return; }
  if (size != IMAGE_SIZE) { Serial.println("ERR size"); return; }
  if (FFat.freeBytes() < IMAGE_SIZE + 4096) { Serial.println("ERR full"); return; }
  String path = String(DIR_CARDS) + "/" + cleanName(name);
  File f = FFat.open(path, "w");
  if (!f) { Serial.println("ERR create"); return; }
  Serial.println("OK");
  static uint8_t blk[4096];
  uint32_t crc = 0;
  Serial.setTimeout(3000);
  for (uint32_t pos = 0; pos < size; pos += sizeof blk) {
    size_t want = min((uint32_t)sizeof blk, size - pos);
    if (Serial.readBytes(blk, want) != want) { f.close(); FFat.remove(path); Serial.println("ERR timeout"); return; }
    f.write(blk, want);
    crc = esp_rom_crc32_le(crc, blk, want);
    Serial.write('K');
  }
  f.close();
  if (crc != crcWant) { FFat.remove(path); Serial.println("ERR crc"); return; }
  Serial.println("DONE");
}

static void handleCmd(String l) {
  l.trim();
  if (l == "PING") Serial.println("PONG SST-PROG");
  else if (l == "LIST") {
    File d = FFat.open(DIR_CARDS);
    for (File e = d.openNextFile(); e; e = d.openNextFile()) if (!e.isDirectory()) Serial.printf("FILE %s %u\n", e.name(), (unsigned)e.size());
    Serial.println("END");
  } else if (l.startsWith("DEL ")) {
    FFat.remove(String(DIR_CARDS) + "/" + cleanName(l.substring(4)));
    Serial.println("OK");
  } else if (l.startsWith("PUT ")) {
    char nm[64]; unsigned sz = 0, crc = 0;
    if (sscanf(l.c_str() + 4, "%63s %u %x", nm, &sz, &crc) == 3) serialPut(nm, sz, crc); else Serial.println("ERR args");
  } else if (l.startsWith("BURN ")) {
    String err;
    if (startBurn(l.substring(5), err)) Serial.println("OK started"); else Serial.println("ERR " + err);
  } else if (l.startsWith("PIN ")) {                 // pin test: PIN A5 / PIN D3 / PIN WE / PIN OFF (meter the SST pin)
    String n = l.substring(4); n.trim(); n.toUpperCase();
    putPins(ADDR_PIN, 19, 0); putPins(DATA_PIN, 8, 0); REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << WE_PIN);   // idle: all low, WE# high
    int k = n.substring(1).toInt();
    if (n == "WE") REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << WE_PIN);
    else if (n[0] == 'A' && k >= 0 && k < 19) putPins(ADDR_PIN, 19, 1UL << k);
    else if (n[0] == 'D' && k >= 0 && k < 8) putPins(DATA_PIN, 8, 1UL << k);
    Serial.println("OK " + n);
  } else if (l == "STATUS") {
    Serial.printf("STATUS %d %u %u %s\n", (int)jobState, (unsigned)jobDone, (unsigned)IMAGE_SIZE, jobMsg);
  }
}

static void serialPoll() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleCmd(line); line = ""; }
    else if (c != '\r' && line.length() < 120) line += c;
  }
}

void setup() {
  Serial.setRxBufferSize(16384);
  Serial.begin(921600);
  delay(300);
  initBus();
  if (!FFat.begin(true)) Serial.println("FFat mount failed");
  FFat.mkdir(DIR_CARDS);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("\nSST programmer: join WiFi '%s' (pw %s), open http://%s/\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  { Preferences p; p.begin("wifi", true); String s = p.getString("ssid", ""), pw = p.getString("pass", ""); p.end();
    if (s.length()) { WiFi.begin(s.c_str(), pw.c_str()); Serial.println("joining saved network '" + s + "'"); } }
  if (MDNS.begin("sstprog")) MDNS.addService("http", "tcp", 80);
  server.on("/wifi", HTTP_GET, []() {
    Preferences p; p.begin("wifi", true); String s = p.getString("ssid", ""); p.end();
    bool ok = WiFi.status() == WL_CONNECTED;
    server.send(200, "application/json", "{\"ssid\":\"" + jsonEscape(s) + "\",\"connected\":" + (ok ? "true" : "false") +
                                             ",\"ip\":\"" + (ok ? WiFi.localIP().toString() : String("")) + "\"}");
  });
  server.on("/wifi", HTTP_POST, []() {
    String s = server.arg("ssid"), pw = server.arg("pass");
    if (s.length()) { Preferences p; p.begin("wifi", false); p.putString("ssid", s); p.putString("pass", pw); p.end();
      WiFi.disconnect(); WiFi.begin(s.c_str(), pw.c_str()); }
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/list", HTTP_GET, []() {
    String o = "{\"cards\":[";
    File d = FFat.open(DIR_CARDS); bool first = true;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (e.isDirectory()) continue;
      if (!first) o += ","; first = false;
      o += "{\"name\":\"" + jsonEscape(e.name()) + "\",\"size\":" + String((unsigned)e.size()) + "}";
    }
    o += "],\"free_kb\":" + String((unsigned)(FFat.freeBytes() / 1024)) + "}";
    server.send(200, "application/json", o);
  });
  server.on("/upload", HTTP_POST, []() {
    if (!uploadOk) server.send(400, "text/plain", "upload failed: " + uploadMsg);
    else { server.sendHeader("Location", "/"); server.send(303); }
  }, handleUpload);
  server.on("/del", HTTP_GET, []() {
    String n = cleanName(server.arg("f"));
    FFat.remove(String(DIR_CARDS) + "/" + n);
    server.send(200, "text/plain", "ok");
  });
  server.on("/burn", HTTP_GET, []() {
    String err;
    if (startBurn(server.arg("f"), err)) server.send(200, "application/json", "{\"ok\":1}");
    else server.send(200, "application/json", "{\"error\":\"" + err + "\"}");
  });
  server.on("/status", HTTP_GET, []() {
    server.send(200, "application/json", "{\"state\":" + String((int)jobState) + ",\"done\":" + String((unsigned)jobDone) +
                                             ",\"total\":" + String((unsigned)IMAGE_SIZE) + ",\"msg\":\"" + jsonEscape(jobMsg) + "\"}");
  });
  server.begin();
}

void loop() {
  static bool wasUp = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) { wasUp = up; if (up) Serial.printf("home WiFi up: http://%s/ or http://sstprog.local/\n", WiFi.localIP().toString().c_str()); }
  serialPoll();
  server.handleClient();
  delay(1);
}

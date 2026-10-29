/*
 * NeuralNexusScreen - ESP32 bridge between a mobile app (over Wi-Fi) and a
 * DWIN DGUS II screen (DMG80480T070_15WTR, T5L) over UART.
 *
 *  Phone  --Wi-Fi-->  ESP32 (SoftAP 192.168.4.1, web app + REST API)  --UART/MAX3232-->  DWIN
 *
 * Screen side (from the DGUS project):
 *   Text Display control, VP 0x2000, 160 bytes, ASCII (0# font), page 0.
 *   Write frame:  5A A5 LEN 82 20 00 <text> FF FF      reply: 5A A5 03 82 4F 4B ("OK")
 *
 * Board: "ESP32 Dev Module" (ESP32-DevKit V1 / ZY-ESP32), core esp32 3.x
 * No external libraries needed.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <vector>
#include "app_html.h"
#include "diag_html.h"
#include "screen_text.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define FW_VERSION      "2.1.0"
#define VP_TEXT         0x2000
#define TEXT_MAX        160      // bytes on the label (32 cols x 5 rows)
#define STORE_MAX       320      // bytes kept per message (UTF-8: Arabic letters are 2 bytes)
#define VP_SP           0x3000   // description pointer of the text control (font/size set live)
#define MAX_MESSAGES    30
#define MAX_SCHEDULES   20
#define LOG_SIZE        25

// ---------------------------------------------------------------- globals
WebServer   server(80);
DNSServer   dns;
Preferences prefs;
HardwareSerial ScreenSerial(2);

String apSsid = "NeuralNexus-Screen";
String apPass = "12345678";
String bootReason = "?";

// screen link
int  scrRx = -1, scrTx = -1;
long scrBaud = 115200;
bool scrInv = false;              // inverted UART levels (TTL screen behind a MAX3232)
bool scrCrc = false;              // DWIN CRC16 mode
bool scrFound = false;            // screen answered our read request
bool scrBlind = false;            // not answering, but we still send (RX line may be broken)
bool usingUart0 = false;          // screen wired to GPIO1/3 -> no debug prints
uint16_t scrVersion = 0;
unsigned long lastScreenOkMs = 0;
uint32_t framesSent = 0, framesAcked = 0;
uint8_t  brightness = 100;

// what is on the screen now
String   curText = "";
String   curSource = "boot";
uint32_t curUntil = 0;            // epoch when a timed message ends (0 = no end)
unsigned long curUntilMs = 0;     // same, in millis (works without clock)

// clock (synced from the phone)
bool     timeSynced = false;
uint32_t epochBase = 0;
unsigned long epochBaseMs = 0;

struct Msg   { uint16_t id; bool pinned; String text; };
struct Sched { uint16_t id; uint32_t at; uint32_t dur; String text; bool done; };

Msg   msgs[MAX_MESSAGES];   int msgCount = 0;
Sched scheds[MAX_SCHEDULES]; int schedCount = 0;
uint16_t nextId = 1;

String logBuf[LOG_SIZE]; int logHead = 0, logLen = 0;

// ---------------------------------------------------------------- helpers
void dbg(const String &s) { if (!usingUart0) Serial.println(s); }

uint32_t nowEpoch() {
  if (!timeSynced) return 0;
  return epochBase + (millis() - epochBaseMs) / 1000;
}

void addLog(const String &s) {
  String line = String(millis() / 1000) + "s  " + s;
  logBuf[logHead] = line;
  logHead = (logHead + 1) % LOG_SIZE;
  if (logLen < LOG_SIZE) logLen++;
  dbg(line);
}

String jsonEsc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// keep UTF-8 text (English + Arabic), newline -> space, max STORE_MAX bytes cut on a character boundary
String cleanText(String t) {
  String o; o.reserve(t.length());
  size_t i = 0, n = t.length();
  while (i < n) {
    uint8_t c = t[i];
    int k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!k || i + k > n) { i++; continue; }
    if (k == 1) {
      if (c == '\r') { i++; continue; }
      if (c == '\n' || c == '\t') c = ' ';
      if (c < 0x20 || c == 0x7F) { i++; continue; }
    }
    if (o.length() + k > STORE_MAX) break;
    if (k == 1) o += (char)c; else o += t.substring(i, i + k);
    i += k;
  }
  o.trim();
  return o;
}

// ---------------------------------------------------------------- DWIN UART
// Wiring of this carrier board (traced from the PCB photo): the MAX3232 is on the ESP32's
// RX0 (GPIO3) / TX0 (GPIO1) pins = the same pins as the USB-serial chip.
//  -> The screen can only ANSWER when the USB cable is unplugged (the USB chip also drives RX0).
//  -> Sending to the screen (TX0) works either way.
#define DEF_RX 3
#define DEF_TX 1

bool uartOpen = false;
bool dualBlind = false;           // blind mode: send every frame on UART2 (16/17) AND UART0 (TX0)

// GPIO3/1 are UART0 (USB). If the screen is wired there we must release Serial first.
void releaseUart0() { if (!usingUart0) { Serial.flush(); Serial.end(); usingUart0 = true; } }
void restoreUart0() { if (usingUart0) { Serial.begin(115200); Serial.setTimeout(60); usingUart0 = false; } }

void screenBegin(int rx, int tx, long baud, bool inv) {
  if (uartOpen) ScreenSerial.end();
  if (dualBlind) { dualBlind = false; restoreUart0(); }
  if (rx == 3 || tx == 1 || rx == 1 || tx == 3) releaseUart0(); else restoreUart0();
  ScreenSerial.begin(baud, SERIAL_8N1, rx, tx, inv);
  uartOpen = true;
}
void screenEnd() {
  if (uartOpen) ScreenSerial.end();
  uartOpen = false;
  if (dualBlind) dualBlind = false;
  restoreUart0();
}
void drainScreen() {
  while (uartOpen && ScreenSerial.available()) ScreenSerial.read();
  while (dualBlind && Serial.available()) Serial.read();
}

// Modbus CRC16 (DWIN CRC mode), sent low byte first
uint16_t crc16(const uint8_t *d, int n) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}

// send 5A A5 LEN <body> [CRC]
void sendFrame(const uint8_t *body, int n) {
  uint8_t hdr[3] = {0x5A, 0xA5, (uint8_t)(n + (scrCrc ? 2 : 0))};
  ScreenSerial.write(hdr, 3);
  ScreenSerial.write(body, n);
  uint8_t cc[2] = {0, 0};
  if (scrCrc) { uint16_t c = crc16(body, n); cc[0] = c; cc[1] = c >> 8; ScreenSerial.write(cc, 2); }
  ScreenSerial.flush();
  if (dualBlind) {                 // same frame on TX0 too (board may wire the MAX3232 to RX0/TX0)
    Serial.write(hdr, 3); Serial.write(body, n); if (scrCrc) Serial.write(cc, 2); Serial.flush();
  }
}

// wait for a frame starting 5A A5; returns length of payload copied into buf.
// In dual blind mode it listens on both RX16 (UART2) and RX0 (UART0); lastFromUart0 tells which.
bool lastFromUart0 = false;
int readFrame(uint8_t *buf, int maxLen, unsigned long timeoutMs) {
  unsigned long t0 = millis();
  int state = 0, need = 0, got = 0;
  while (millis() - t0 < timeoutMs) {
    for (;;) {
      Stream *src = nullptr;
      if (ScreenSerial.available()) { src = &ScreenSerial; lastFromUart0 = false; }
      else if (dualBlind && Serial.available()) { src = &Serial; lastFromUart0 = true; }
      if (!src) break;
      uint8_t b = src->read();
      if (state == 0) { if (b == 0x5A) state = 1; }
      else if (state == 1) { state = (b == 0xA5) ? 2 : (b == 0x5A ? 1 : 0); }
      else if (state == 2) { need = b; got = 0; state = 3; if (need == 0 || need > maxLen) state = 0; }
      else { buf[got++] = b; if (got >= need) return got; }
    }
    delay(1);
  }
  return -1;
}

// read the screen's version register (0x000F) -> true if it answered
void lockDualTo(bool uart0);
bool screenPing(uint16_t *ver, unsigned long timeoutMs = 60) {
  if (!uartOpen) return false;
  const uint8_t q[] = {0x83, 0x00, 0x0F, 0x01};
  drainScreen();
  sendFrame(q, sizeof(q));
  uint8_t buf[32];
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int n = readFrame(buf, sizeof(buf), timeoutMs);
    if (n < 0) break;
    if (n >= 6 && buf[0] == 0x83 && buf[1] == 0x00 && buf[2] == 0x0F) {
      if (ver) *ver = (buf[4] << 8) | buf[5];
      if (dualBlind) lockDualTo(lastFromUart0);
      return true;
    }
  }
  return false;
}

// write bytes to a VP; returns true if the screen replied "OK"
bool screenWrite(uint16_t vp, const uint8_t *data, int len, bool waitAck = true) {
  if (!uartOpen || !(scrFound || scrBlind)) return false;
  uint8_t body[TEXT_MAX + 16];
  body[0] = 0x82; body[1] = vp >> 8; body[2] = vp;
  memcpy(body + 3, data, len);
  drainScreen();
  sendFrame(body, len + 3);
  framesSent++;
  if (!waitAck) return true;
  uint8_t buf[16];
  int n = readFrame(buf, sizeof(buf), 80);
  bool ok = (n >= 3 && buf[0] == 0x82 && buf[1] == 0x4F && buf[2] == 0x4B);
  if (ok) { framesAcked++; lastScreenOkMs = millis(); }
  return ok;
}

#define DEFAULT_TEXT "BORG EL ARAB TECHNOLOGICAL UNIVERSITY"
int lastFontMode = -1;
// Text control description (tested on this screen, see DWIN_FONT_FINDINGS):
//   GB2312 encoding, Font0 = 24 (24_ASC.HZK, English 20x40), Font1 = 28 (28_AR.HZK, Arabic 40x40), black text.
// The same setup shows English, Arabic and mixed text, so it never has to switch.
#define TEXT_COLOR 0x0000
bool screenSetFontMode(int mode) {
  (void)mode;
  uint16_t w[13] = {VP_TEXT, 78, 145, TEXT_COLOR, 78, 145, 722, 367, TEXT_MAX,
                    (uint16_t)((24 << 8) | 28), (uint16_t)((40 << 8) | 40), 0x0100, 0};
  uint8_t b[26];
  for (int i = 0; i < 13; i++) { b[2 * i] = w[i] >> 8; b[2 * i + 1] = w[i] & 0xFF; }
  bool ok = screenWrite(VP_SP, b, sizeof(b));
  lastFontMode = 1;
  return ok;
}

bool screenShowText(const String &raw) {
  bool ar = false;
  String t = stext::build(raw, ar);
  screenSetFontMode(ar ? 1 : 0);              // always: the screen may have rebooted since last time
  uint8_t data[TEXT_MAX + 2];
  int n = t.length(); if (n > TEXT_MAX) n = TEXT_MAX;
  memcpy(data, t.c_str(), n);
  data[n] = 0xFF; data[n + 1] = 0xFF;          // end-of-text marker
  return screenWrite(VP_TEXT, data, n + 2);
}

bool screenSetBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  uint8_t d[2] = {pct, pct};
  return screenWrite(0x0082, d, 2);
}

bool screenSetPage(uint8_t page) {
  uint8_t d[4] = {0x5A, 0x01, 0x00, page};
  return screenWrite(0x0084, d, 4);
}

bool tryLink(int rx, int tx, long baud, bool inv, bool crc) {
  bool oldCrc = scrCrc; scrCrc = crc;
  screenBegin(rx, tx, baud, inv);
  delay(10);
  uint16_t v = 0;
  if (screenPing(&v, 50) || screenPing(&v, 50)) {
    scrRx = rx; scrTx = tx; scrBaud = baud; scrInv = inv; scrFound = true; scrBlind = false; scrVersion = v;
    lastScreenOkMs = millis();
    return true;
  }
  scrCrc = oldCrc;
  screenEnd();
  if (!(rx == 3 || tx == 1 || rx == 1 || tx == 3)) { pinMode(rx, INPUT); pinMode(tx, INPUT); }
  return false;
}

// open the saved/default config and send without waiting for replies
void enterBlind(bool dual = false) {
  int rx = scrRx >= 0 ? scrRx : DEF_RX, tx = scrRx >= 0 ? scrTx : DEF_TX;
  if (dual) { rx = 16; tx = 17; }  // UART2 on 16/17 + UART0 (TX0) below = both possible wirings
  scrRx = rx; scrTx = tx;
  screenBegin(rx, tx, scrBaud, scrInv);
  if (dual) {                      // also drive TX0 at the same settings, stop debug prints on it
    releaseUart0(); Serial.begin(scrBaud, SERIAL_8N1, 3, 1, scrInv); dualBlind = true;
    scrRx = DEF_RX; scrTx = DEF_TX;
  }
  scrFound = false; scrBlind = true;
  if (dual) addLog("No reply -> sending on BOTH TX0 and TX17 @" + String(scrBaud));
  addLog("No reply from screen -> sending anyway on RX=" + String(rx) + " TX=" + String(tx) + " @" + String(scrBaud) +
         (scrInv ? " inverted" : "") + (scrCrc ? " CRC" : ""));
}

void finishFound();
// the screen answered while we were sending on both ports: keep only the port it answered on
void lockDualTo(bool uart0) {
  long b = scrBaud; bool inv = scrInv;
  dualBlind = false;
  if (uart0) { Serial.end(); ScreenSerial.end(); uartOpen = false; usingUart0 = true; screenBegin(3, 1, b, inv); scrRx = 3; scrTx = 1; }
  else { Serial.end(); usingUart0 = true; restoreUart0(); scrRx = 16; scrTx = 17; }
  scrFound = true; scrBlind = false; lastScreenOkMs = millis();
  finishFound();
}

// ---- screen search (non-blocking: one candidate per loop, web server stays responsive)
struct Cand { int8_t rx, tx; long baud; bool inv, crc; };
std::vector<Cand> cands;
size_t candIdx = 0;
bool searching = false;
unsigned long searchDoneMs = 0;

void buildCands(bool full) {
  cands.clear();
  if (scrRx >= 0) cands.push_back({(int8_t)scrRx, (int8_t)scrTx, scrBaud, scrInv, scrCrc});
  const long bauds[] = {115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600};
  // UART0 (RX0/TX0) first: this carrier board appears to wire the MAX3232 there.
  // NOTE: only works with the USB cable unplugged (the USB chip also drives RX0).
  for (long b : bauds) for (int inv = 0; inv < 2; inv++) for (int crc = 0; crc < 2; crc++)
    cands.push_back({3, 1, b, inv == 1, crc == 1});
  for (int inv = 0; inv < 2; inv++) cands.push_back({1, 3, 115200, inv == 1, false});   // swapped wiring
  const int main2[][2] = {{16, 17}, {17, 16}};
  for (auto &p : main2) for (long b : bauds) for (int inv = 0; inv < 2; inv++) for (int crc = 0; crc < 2; crc++)
    cands.push_back({(int8_t)p[0], (int8_t)p[1], b, inv == 1, crc == 1});
  const int common[][2] = {{26,27},{27,26},{25,26},{26,25},{32,33},{33,32},{18,19},{19,18},{21,22},{22,21},
                           {4,2},{2,4},{13,12},{12,13},{14,27},{27,14},{5,18},{18,5},{23,22},{22,23},{4,15},{15,4}};
  for (auto &p : common) for (int inv = 0; inv < 2; inv++) cands.push_back({(int8_t)p[0], (int8_t)p[1], 115200, inv == 1, false});
  if (!full) return;
  for (long b : bauds) if (b != 115200) for (auto &p : common) cands.push_back({(int8_t)p[0], (int8_t)p[1], b, false, false});
  const int pins[] = {2,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33};
  for (int rx : pins) for (int tx : pins) if (rx != tx) cands.push_back({(int8_t)rx, (int8_t)tx, 115200, false, false});
}

void startSearch(bool full) {
  screenEnd();
  scrFound = false; scrBlind = false;
  buildCands(full);
  candIdx = 0; searching = true;
  addLog(String(full ? "Full" : "Quick") + " screen search started (" + String(cands.size()) + " combinations)");
}

void finishFound() {
  prefs.putInt("rx", scrRx); prefs.putInt("tx", scrTx); prefs.putLong("baud", scrBaud);
  prefs.putBool("inv", scrInv); prefs.putBool("crc", scrCrc);
  addLog("Screen found RX=" + String(scrRx) + " TX=" + String(scrTx) + " @" + String(scrBaud) +
         (scrInv ? " inverted" : "") + (scrCrc ? " CRC" : "") + " ver=0x" + String(scrVersion, HEX));
}

void stepSearch() {
  if (!searching) return;
  if (candIdx >= cands.size()) {
    searching = false; searchDoneMs = millis();
    addLog("Screen did NOT answer. If the ESP32 USB cable is plugged in, unplug it (it blocks RX0) and reboot.");
    scrRx = -1; scrBaud = 115200; scrInv = false; scrCrc = false;
    enterBlind(true);
    return;
  }
  Cand c = cands[candIdx++];
  if (tryLink(c.rx, c.tx, c.baud, c.inv, c.crc)) { searching = false; searchDoneMs = millis(); finishFound(); }
}

// ---------------------------------------------------------------- storage
void saveMessages() {
  String s;
  for (int i = 0; i < msgCount; i++)
    s += String(msgs[i].id) + "\x1F" + (msgs[i].pinned ? "1" : "0") + "\x1F" + msgs[i].text + "\x1E";
  prefs.putBytes("msgs", s.c_str(), s.length() + 1);
  prefs.putUShort("nextId", nextId);
}

void saveSchedules() {
  String s;
  for (int i = 0; i < schedCount; i++)
    s += String(scheds[i].id) + "\x1F" + String(scheds[i].at) + "\x1F" + String(scheds[i].dur) + "\x1F" + scheds[i].text + "\x1E";
  prefs.putBytes("scheds", s.c_str(), s.length() + 1);
  prefs.putUShort("nextId", nextId);
}

String readBlob(const char *key) {
  size_t n = prefs.getBytesLength(key);
  if (n == 0) return "";
  char *b = (char *)malloc(n + 1);
  prefs.getBytes(key, b, n); b[n] = 0;
  String s(b); free(b); return s;
}

void splitFields(const String &rec, String *f, int maxF) {
  int start = 0, k = 0;
  for (int i = 0; i <= (int)rec.length() && k < maxF; i++) {
    if (i == (int)rec.length() || rec[i] == '\x1F') { f[k++] = rec.substring(start, i); start = i + 1; }
  }
}

void loadAll() {
  nextId = prefs.getUShort("nextId", 1);
  apSsid = prefs.getString("ssid", apSsid);
  apPass = prefs.getString("pass", apPass);
  scrRx = prefs.getInt("rx", -1); scrTx = prefs.getInt("tx", -1); scrBaud = prefs.getLong("baud", 115200);
  scrInv = prefs.getBool("inv", false); scrCrc = prefs.getBool("crc", false);
  brightness = prefs.getUChar("bright", 100);

  String s = readBlob("msgs"); msgCount = 0; int st = 0;
  for (int i = 0; i < (int)s.length() && msgCount < MAX_MESSAGES; i++) if (s[i] == '\x1E') {
    String f[3]; splitFields(s.substring(st, i), f, 3); st = i + 1;
    msgs[msgCount++] = {(uint16_t)f[0].toInt(), f[1] == "1", f[2]};
  }
  s = readBlob("scheds"); schedCount = 0; st = 0;
  for (int i = 0; i < (int)s.length() && schedCount < MAX_SCHEDULES; i++) if (s[i] == '\x1E') {
    String f[4]; splitFields(s.substring(st, i), f, 4); st = i + 1;
    scheds[schedCount++] = {(uint16_t)f[0].toInt(), (uint32_t)f[1].toInt(), (uint32_t)f[2].toInt(), f[3], false};
  }
}

int findMsg(uint16_t id) { for (int i = 0; i < msgCount; i++) if (msgs[i].id == id) return i; return -1; }
int pinnedIndex() { for (int i = 0; i < msgCount; i++) if (msgs[i].pinned) return i; return -1; }

// ---------------------------------------------------------------- display logic
bool showOnScreen(const String &raw, const String &source, uint32_t durSec) {
  String t = cleanText(raw);
  bool ok = screenShowText(t);
  curText = t; curSource = source;
  if (durSec > 0) {
    curUntilMs = millis() + durSec * 1000UL;
    curUntil = timeSynced ? nowEpoch() + durSec : 0;
  } else { curUntilMs = 0; curUntil = 0; }
  addLog(String(ok ? "Shown (screen OK): \"" : "Sent (NO reply from screen): \"") + t.substring(0, 40) + "\"");
  return ok;
}

// what to show when nothing timed is active
void showDefault() {
  int p = pinnedIndex();
  if (p >= 0) showOnScreen(msgs[p].text, "pinned", 0);
  else        showOnScreen(DEFAULT_TEXT, "default", 0);
}

void tickScheduler() {
  // timed message finished?
  if (curUntilMs && (long)(millis() - curUntilMs) >= 0) {
    curUntilMs = 0; curUntil = 0;
    addLog("Timed message ended");
    showDefault();
  }
  if (!timeSynced) return;
  uint32_t now = nowEpoch();
  for (int i = 0; i < schedCount; i++) {
    if (now >= scheds[i].at) {
      Sched s = scheds[i];
      for (int j = i; j < schedCount - 1; j++) scheds[j] = scheds[j + 1];
      schedCount--; saveSchedules();
      addLog("Schedule #" + String(s.id) + " fired");
      showOnScreen(s.text, "schedule", s.dur);
      break;
    }
  }
}

// ---------------------------------------------------------------- HTTP
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}
void sendJson(const String &j, int code = 200) { cors(); server.send(code, "application/json", j); }
void ok(const String &extra = "") { sendJson(String("{\"ok\":true") + (extra.length() ? "," + extra : "") + "}"); }
void fail(const String &msg, int code = 400) { sendJson("{\"ok\":false,\"error\":\"" + jsonEsc(msg) + "\"}", code); }

String msgsJson() {
  String j = "[";
  for (int i = 0; i < msgCount; i++) {
    if (i) j += ",";
    j += "{\"id\":" + String(msgs[i].id) + ",\"pinned\":" + (msgs[i].pinned ? "true" : "false") +
         ",\"text\":\"" + jsonEsc(msgs[i].text) + "\"}";
  }
  return j + "]";
}

String schedsJson() {
  String j = "[";
  for (int i = 0; i < schedCount; i++) {
    if (i) j += ",";
    j += "{\"id\":" + String(scheds[i].id) + ",\"at\":" + String(scheds[i].at) + ",\"dur\":" + String(scheds[i].dur) +
         ",\"text\":\"" + jsonEsc(scheds[i].text) + "\"}";
  }
  return j + "]";
}

String statusJson() {
  bool alive = scrFound && (millis() - lastScreenOkMs < 15000);
  int p = pinnedIndex();
  String j = "{\"ok\":true,\"fw\":\"" FW_VERSION "\",\"boot\":\"" + bootReason + "\",\"uptime\":" + String(millis() / 1000);
  j += ",\"screen\":{\"found\":" + String(scrFound ? "true" : "false") + ",\"alive\":" + (alive ? "true" : "false") +
       ",\"rx\":" + String(scrRx) + ",\"tx\":" + String(scrTx) + ",\"baud\":" + String(scrBaud) +
       ",\"version\":" + String(scrVersion) + ",\"sent\":" + String(framesSent) + ",\"acked\":" + String(framesAcked) +
       ",\"brightness\":" + String(brightness) + ",\"blind\":" + (scrBlind ? "true" : "false") +
       ",\"inv\":" + (scrInv ? "true" : "false") + ",\"crc\":" + (scrCrc ? "true" : "false") +
       ",\"searching\":" + (searching ? "true" : "false") + ",\"progress\":" + String(candIdx) + ",\"total\":" + String(cands.size()) + "}";
  j += ",\"wifi\":{\"ssid\":\"" + jsonEsc(apSsid) + "\",\"ip\":\"" + WiFi.softAPIP().toString() +
       "\",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}";
  j += ",\"time\":{\"synced\":" + String(timeSynced ? "true" : "false") + ",\"epoch\":" + String(nowEpoch()) + "}";
  long left = curUntilMs ? (long)(curUntilMs - millis()) / 1000 : 0;
  j += ",\"current\":{\"text\":\"" + jsonEsc(curText) + "\",\"source\":\"" + curSource + "\",\"secondsLeft\":" + String(left < 0 ? 0 : left) + "}";
  j += ",\"pinnedId\":" + String(p >= 0 ? msgs[p].id : 0);
  j += ",\"counts\":{\"messages\":" + String(msgCount) + ",\"schedules\":" + String(schedCount) + "}}";
  return j;
}

uint32_t argU(const char *n, uint32_t def = 0) { return server.hasArg(n) ? (uint32_t)strtoul(server.arg(n).c_str(), 0, 10) : def; }

// ---------------------------------------------------------------- diagnostics
// put the link back the way it was before a diagnostic test
void restoreLink() {
  if (scrFound) { bool c = scrCrc; screenBegin(scrRx, scrTx, scrBaud, scrInv); scrCrc = c; }
  else { scrBaud = 115200; scrInv = false; scrCrc = false; enterBlind(true); }
}

// open one UART config, send a version read, capture EVERY byte that comes back for 90 ms
String probeOne(int rx, int tx, long baud, bool inv, bool crc, bool &valid, uint16_t &ver) {
  bool oc = scrCrc; scrCrc = crc;
  screenBegin(rx, tx, baud, inv);
  delay(8); drainScreen();
  const uint8_t q[] = {0x83, 0x00, 0x0F, 0x01};
  sendFrame(q, sizeof(q));
  uint8_t r[40]; int n = 0; unsigned long t0 = millis();
  while (millis() - t0 < 90) { while (ScreenSerial.available() && n < 40) r[n++] = ScreenSerial.read(); delay(1); }
  valid = false;
  for (int i = 0; i + 5 < n; i++)
    if (r[i] == 0x5A && r[i + 1] == 0xA5 && r[i + 2] >= 0x06 && r[i + 3] == 0x83 && r[i + 4] == 0x00 && r[i + 5] == 0x0F) {  // len>=6: a real reply, not our own 04-length request echoed back
      valid = true; ver = (i + 8 < n) ? (r[i + 7] << 8) | r[i + 8] : 0; break;
    }
  scrCrc = oc;
  String h; char t[4];
  for (int i = 0; i < n; i++) { sprintf(t, "%02X ", r[i]); h += t; }
  h.trim();
  return h;
}

String diagScanJson() {
  struct P { int rx, tx; const char *name; } ports[] = {{3, 1, "RX0/TX0"}, {16, 17, "RX16/TX17"}, {1, 3, "swapped 0"}, {17, 16, "swapped 16/17"}};
  const long bauds[] = {115200, 9600, 57600, 38400, 19200, 230400};
  searching = false;
  String j = "["; bool first = true; int hits = 0;
  int bestRx = -1, bestTx = -1; long bestB = 0; bool bestInv = false, bestCrc = false; uint16_t bestVer = 0;
  for (auto &p : ports) for (long b : bauds) for (int inv = 0; inv < 2; inv++) for (int crc = 0; crc < (b == 115200 ? 2 : 1); crc++) {
    bool valid; uint16_t ver = 0;
    String h = probeOne(p.rx, p.tx, b, inv, crc, valid, ver);
    if (valid && bestRx < 0) { bestRx = p.rx; bestTx = p.tx; bestB = b; bestInv = inv; bestCrc = crc; bestVer = ver; }
    if (h.length()) hits++;
    if (!first) j += ","; first = false;
    j += "{\"port\":\"" + String(p.name) + "\",\"rx\":" + p.rx + ",\"tx\":" + p.tx + ",\"baud\":" + b +
         ",\"inv\":" + (inv ? "true" : "false") + ",\"crc\":" + (crc ? "true" : "false") +
         ",\"valid\":" + (valid ? "true" : "false") + ",\"rxBytes\":\"" + h + "\"}";
  }
  j += "]";
  if (bestRx >= 0) {
    scrRx = bestRx; scrTx = bestTx; scrBaud = bestB; scrInv = bestInv; scrCrc = bestCrc; scrVersion = bestVer;
    scrFound = true; scrBlind = false; lastScreenOkMs = millis(); finishFound();
  }
  restoreLink();
  addLog("Diagnostic scan done: " + String(bestRx >= 0 ? "screen ANSWERED" : "no valid answer") + ", configs with any bytes: " + hits);
  return "{\"ok\":true,\"found\":" + String(bestRx >= 0 ? "true" : "false") + ",\"anyBytes\":" + hits + ",\"items\":" + j + "}";
}

void setupRoutes() {
  server.on("/diag", HTTP_GET, []() { cors(); server.send_P(200, "text/html; charset=utf-8", DIAG_HTML); });

  // live font test: write the text control description with any font/size/encoding, then a raw text
  server.on("/api/diag/font", HTTP_POST, []() {
    uint16_t f0 = argU("f0", 24), f1 = argU("f1", 28), x = argU("x", 40), y = argU("y", 40), enc = argU("enc", 1);
    uint16_t w[13] = {VP_TEXT, 78, 145, (uint16_t)argU("color", TEXT_COLOR), 78, 145, 722, 367, TEXT_MAX,
                      (uint16_t)((f0 << 8) | f1), (uint16_t)((x << 8) | y), (uint16_t)(enc << 8), 0};
    uint8_t b[26]; for (int i = 0; i < 13; i++) { b[2 * i] = w[i] >> 8; b[2 * i + 1] = w[i] & 0xFF; }
    bool a1 = screenWrite(VP_SP, b, sizeof(b));
    lastFontMode = -1;
    String t = server.hasArg("text") ? server.arg("text") : String("ABC abc 123");
    int n = t.length(); if (n > TEXT_MAX) n = TEXT_MAX;
    uint8_t d[TEXT_MAX + 2]; memcpy(d, t.c_str(), n); d[n] = 0xFF; d[n + 1] = 0xFF;
    bool a2 = screenWrite(VP_TEXT, d, n + 2);
    curText = t; curSource = "font test";
    addLog("Font test f0=" + String(f0) + " f1=" + String(f1) + " " + String(x) + "x" + String(y) + " enc=" + String(enc) + (a2 ? " ACK" : " no ack"));
    ok(String("\"ack\":") + (a1 && a2 ? "true" : "false"));
  });

  server.on("/api/diag/scan", HTTP_POST, []() { sendJson(diagScanJson()); });

  // send a text ONLY on one path, regardless of detection: path=tx0|tx17, inv=0|1, baud
  server.on("/api/diag/write", HTTP_POST, []() {
    bool tx0 = server.arg("path") != "tx17";
    bool inv = server.arg("inv") == "1";
    long b = server.hasArg("baud") ? server.arg("baud").toInt() : 115200;
    String t = cleanText(server.arg("text"));
    searching = false;
    bool oc = scrCrc; scrCrc = server.arg("crc") == "1";
    if (tx0) screenBegin(3, 1, b, inv); else screenBegin(16, 17, b, inv);
    delay(5); drainScreen();
    uint8_t body[TEXT_MAX + 8]; int n = t.length(); if (n > TEXT_MAX) n = TEXT_MAX;
    body[0] = 0x82; body[1] = VP_TEXT >> 8; body[2] = VP_TEXT & 0xFF;
    memcpy(body + 3, t.c_str(), n); body[3 + n] = 0xFF; body[4 + n] = 0xFF;
    sendFrame(body, n + 5);
    uint8_t r[16]; int m = 0; unsigned long t0 = millis();
    while (millis() - t0 < 120) { while (ScreenSerial.available() && m < 16) r[m++] = ScreenSerial.read(); delay(1); }
    String h; char tt[4]; for (int i = 0; i < m; i++) { sprintf(tt, "%02X ", r[i]); h += tt; } h.trim();
    bool ack = h.indexOf("82 4F 4B") >= 0;
    scrCrc = oc;
    framesSent++; if (ack) framesAcked++;
    curText = t; curSource = "test";
    addLog(String("Diag write on ") + (tx0 ? "TX0" : "TX17") + (inv ? " inv" : "") + " @" + b + (ack ? " -> ACK" : " -> no ack") + (m ? " rx: " + h : ""));
    restoreLink();
    ok(String("\"ack\":") + (ack ? "true" : "false") + ",\"rxBytes\":\"" + h + "\"");
  });

  server.on("/", HTTP_GET, []() { cors(); server.send_P(200, "text/html; charset=utf-8", APP_HTML); });

  server.on("/api/status", HTTP_GET, []() { sendJson(statusJson()); });

  server.on("/api/time", HTTP_POST, []() {
    uint32_t e = argU("epoch");
    if (e < 1600000000UL) return fail("bad epoch");
    epochBase = e; epochBaseMs = millis();
    if (!timeSynced) addLog("Clock synced from phone");
    timeSynced = true; ok();
  });

  server.on("/api/send", HTTP_POST, []() {
    String t = server.arg("text");
    uint32_t dur = argU("dur");
    if (server.arg("save") == "1" && msgCount < MAX_MESSAGES) {
      msgs[msgCount++] = {nextId++, false, cleanText(t)}; saveMessages();
    }
    bool acked = showOnScreen(t, "app", dur);
    ok(String("\"acked\":") + (acked ? "true" : "false") + ",\"shown\":\"" + jsonEsc(curText) + "\"");
  });

  server.on("/api/clear", HTTP_POST, []() {
    bool acked = showOnScreen(DEFAULT_TEXT, "default", 0);
    ok(String("\"acked\":") + (acked ? "true" : "false"));
  });

  server.on("/api/messages", HTTP_GET, []() { sendJson("{\"ok\":true,\"items\":" + msgsJson() + "}"); });

  server.on("/api/messages/add", HTTP_POST, []() {
    if (msgCount >= MAX_MESSAGES) return fail("message list is full (30)");
    String t = cleanText(server.arg("text"));
    if (!t.length()) return fail("empty text");
    msgs[msgCount++] = {nextId++, false, t}; saveMessages();
    ok("\"id\":" + String(nextId - 1));
  });

  server.on("/api/messages/update", HTTP_POST, []() {
    int i = findMsg(argU("id")); if (i < 0) return fail("not found", 404);
    msgs[i].text = cleanText(server.arg("text")); saveMessages();
    if (msgs[i].pinned && curSource == "pinned") showDefault();
    ok();
  });

  server.on("/api/messages/delete", HTTP_POST, []() {
    int i = findMsg(argU("id")); if (i < 0) return fail("not found", 404);
    bool wasPinned = msgs[i].pinned;
    for (int j = i; j < msgCount - 1; j++) msgs[j] = msgs[j + 1];
    msgCount--; saveMessages();
    if (wasPinned && curSource == "pinned") showDefault();
    ok();
  });

  server.on("/api/messages/pin", HTTP_POST, []() {
    int i = findMsg(argU("id")); if (i < 0) return fail("not found", 404);
    bool pin = server.arg("pin") != "0";
    for (int j = 0; j < msgCount; j++) msgs[j].pinned = false;
    msgs[i].pinned = pin; saveMessages();
    if (!curUntilMs) showDefault();          // show the pinned message now (unless a timed one runs)
    ok();
  });

  server.on("/api/messages/show", HTTP_POST, []() {
    int i = findMsg(argU("id")); if (i < 0) return fail("not found", 404);
    bool acked = showOnScreen(msgs[i].text, "saved", argU("dur"));
    ok(String("\"acked\":") + (acked ? "true" : "false"));
  });

  server.on("/api/schedules", HTTP_GET, []() {
    sendJson("{\"ok\":true,\"now\":" + String(nowEpoch()) + ",\"items\":" + schedsJson() + "}");
  });

  server.on("/api/schedules/add", HTTP_POST, []() {
    if (!timeSynced) return fail("clock not synced yet - reopen the app");
    if (schedCount >= MAX_SCHEDULES) return fail("schedule list is full (20)");
    String t = cleanText(server.arg("text"));
    uint32_t at = argU("at");
    if (!at && server.hasArg("in")) at = nowEpoch() + argU("in");
    if (!t.length() || !at) return fail("text and time are required");
    // keep sorted by time
    int pos = schedCount;
    while (pos > 0 && scheds[pos - 1].at > at) { scheds[pos] = scheds[pos - 1]; pos--; }
    scheds[pos] = {nextId++, at, argU("dur"), t, false}; schedCount++;
    saveSchedules();
    addLog("Scheduled #" + String(nextId - 1) + " in " + String((long)(at - nowEpoch())) + "s");
    ok("\"id\":" + String(nextId - 1));
  });

  server.on("/api/schedules/delete", HTTP_POST, []() {
    uint16_t id = argU("id");
    for (int i = 0; i < schedCount; i++) if (scheds[i].id == id) {
      for (int j = i; j < schedCount - 1; j++) scheds[j] = scheds[j + 1];
      schedCount--; saveSchedules(); return ok();
    }
    fail("not found", 404);
  });

  server.on("/api/screen/test", HTTP_POST, []() {
    if (!uartOpen) return fail("screen port not open - press Detect");
    unsigned long t0 = micros();
    uint16_t v = 0;
    bool alive = screenPing(&v, 120);
    long rtt = (micros() - t0) / 1000;
    if (alive) { lastScreenOkMs = millis(); scrVersion = v; if (!scrFound) { scrFound = true; scrBlind = false; finishFound(); } }
    addLog(String("Screen test: ") + (alive ? "OK " + String(rtt) + "ms" : "NO REPLY"));
    ok(String("\"alive\":") + (alive ? "true" : "false") + ",\"ms\":" + String(rtt) + ",\"version\":" + String(v));
  });

  server.on("/api/screen/detect", HTTP_POST, []() {
    startSearch(true);
    ok("\"started\":true,\"total\":" + String(cands.size()));
  });

  // manual link settings: rx, tx, baud, inv (0/1), crc (0/1)
  server.on("/api/screen/pins", HTTP_POST, []() {
    int rx = server.arg("rx").toInt(), tx = server.arg("tx").toInt();
    long b = server.hasArg("baud") ? server.arg("baud").toInt() : 115200;
    bool inv = server.arg("inv") == "1", crc = server.arg("crc") == "1";
    searching = false;
    bool f = tryLink(rx, tx, b, inv, crc);
    if (f) finishFound();
    else { scrRx = rx; scrTx = tx; scrBaud = b; scrInv = inv; scrCrc = crc;
           prefs.putInt("rx", rx); prefs.putInt("tx", tx); prefs.putLong("baud", b); prefs.putBool("inv", inv); prefs.putBool("crc", crc);
           enterBlind(); }
    showDefault();
    addLog(String("Manual link RX=") + rx + " TX=" + tx + " @" + b + (inv ? " inv" : "") + (crc ? " crc" : "") + (f ? " -> screen answered" : " -> no reply, sending blind"));
    ok(String("\"found\":") + (f ? "true" : "false"));
  });

  server.on("/api/screen/brightness", HTTP_POST, []() {
    brightness = constrain((int)argU("value", 100), 5, 100);
    prefs.putUChar("bright", brightness);
    bool acked = screenSetBrightness(brightness);
    ok(String("\"acked\":") + (acked ? "true" : "false"));
  });

  server.on("/api/screen/testmsg", HTTP_POST, []() {
    String t = "TEST OK - ESP32 -> SCREEN  " + String(millis() / 1000) + "s";
    bool acked = showOnScreen(t, "test", 15);
    ok(String("\"acked\":") + (acked ? "true" : "false"));
  });

  server.on("/api/log", HTTP_GET, []() {
    String j = "{\"ok\":true,\"items\":[";
    for (int i = 0; i < logLen; i++) {
      int k = (logHead - 1 - i + LOG_SIZE) % LOG_SIZE;
      if (i) j += ",";
      j += "\"" + jsonEsc(logBuf[k]) + "\"";
    }
    sendJson(j + "]}");
  });

  server.on("/api/wifi", HTTP_POST, []() {
    String s = server.arg("ssid"), p = server.arg("pass");
    if (s.length() < 2 || s.length() > 31) return fail("Wi-Fi name must be 2-31 characters");
    if (p.length() && (p.length() < 8 || p.length() > 63)) return fail("password must be 8-63 characters");
    prefs.putString("ssid", s); prefs.putString("pass", p);
    ok(); delay(400); ESP.restart();
  });

  server.on("/api/reboot", HTTP_POST, []() { ok(); delay(300); ESP.restart(); });

  // OS connectivity checks: answer "internet OK" so phones stay on this Wi-Fi
  server.on("/generate_204", HTTP_GET, []() { server.send(204); });
  server.on("/gen_204", HTTP_GET, []() { server.send(204); });
  server.on("/hotspot-detect.html", HTTP_GET, []() { server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/library/test/success.html", HTTP_GET, []() { server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/connecttest.txt", HTTP_GET, []() { server.send(200, "text/plain", "Microsoft Connect Test"); });
  server.on("/ncsi.txt", HTTP_GET, []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.on("/success.txt", HTTP_GET, []() { server.send(200, "text/plain", "success\n"); });

  // captive portal: anything else -> the app
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      cors(); server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type"); server.send(204); return;
    }
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // board 5V dips when Wi-Fi starts -> do not reset on brief sags
  Serial.begin(115200);
  Serial.setTimeout(60);
  delay(200);
  dbg("\n=== NeuralNexusScreen " FW_VERSION " ===");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: bootReason = "power-on"; break;
    case ESP_RST_SW: bootReason = "software restart"; break;
    case ESP_RST_PANIC: bootReason = "CRASH (panic)"; break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: bootReason = "CRASH (watchdog)"; break;
    case ESP_RST_BROWNOUT: bootReason = "BROWNOUT (weak 5V supply)"; break;
    case ESP_RST_EXT: bootReason = "reset button"; break;
    default: bootReason = "other";
  }

  prefs.begin("nnscreen", false);
  loadAll();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(apSsid.c_str(), apPass.length() ? apPass.c_str() : nullptr, 6, 0, 4)) {
    apSsid = "NeuralNexus-Screen"; apPass = "12345678";           // saved settings rejected -> safe defaults
    WiFi.softAP(apSsid.c_str(), apPass.c_str(), 6, 0, 4);
  }
  WiFi.setTxPower(WIFI_POWER_11dBm);        // lower peak current (enough for one room)
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  MDNS.begin("screen");
  setupRoutes();
  server.begin();
  addLog("Wi-Fi \"" + apSsid + "\" ready at 192.168.4.1");
  addLog("Boot reason: " + bootReason);

  delay(1500);                       // give the screen time to boot
  startSearch(scrRx < 0);            // never found before -> full search (runs in the background)
}

unsigned long lastTick = 0, lastRetry = 0, lastBeat = 0;

// USB serial commands (Serial Monitor @115200) for testing without a phone:
//   status | say <text> | clear | detect | ping | log | sniff | cfg <rx> <tx> <baud> <inv> <crc> | raw <hex bytes>
String hexs(const uint8_t *b, int n) { String o; char t[4]; for (int i = 0; i < n; i++) { sprintf(t, "%02X ", b[i]); o += t; } return o; }
void handleSerialCmd() {
  if (usingUart0 || !Serial.available()) return;
  String c = Serial.readStringUntil('\n'); c.trim();
  if (!c.length()) return;
  if (c == "status") Serial.println(statusJson());
  else if (c.startsWith("say ")) Serial.println(showOnScreen(c.substring(4), "serial", 0) ? "SCREEN ACK OK" : "SENT, NO ACK FROM SCREEN");
  else if (c == "clear") Serial.println(showOnScreen(DEFAULT_TEXT, "default", 0) ? "SCREEN ACK OK" : "SENT, NO ACK FROM SCREEN");
  else if (c == "detect") startSearch(true);
  else if (c == "ping") { uint16_t v = 0; bool a = screenPing(&v, 150); Serial.println(a ? "SCREEN ALIVE ver=0x" + String(v, HEX) : "SCREEN NO REPLY"); }
  else if (c == "log") { for (int i = logLen - 1; i >= 0; i--) Serial.println(logBuf[(logHead - 1 - i + LOG_SIZE) % LOG_SIZE]); }
  else if (c == "sniff") {           // print anything the screen sends for 5 s (touch the screen)
    Serial.println("listening 5s...");
    unsigned long t0 = millis(); uint8_t b[64]; int n = 0;
    while (millis() - t0 < 5000) { while (uartOpen && ScreenSerial.available() && n < 64) b[n++] = ScreenSerial.read(); delay(2); }
    Serial.println(n ? "RX: " + hexs(b, n) : "nothing received");
  }
  else if (c.startsWith("cfg ")) {   // cfg 16 17 115200 0 0
    int a[5] = {0, 0, 115200, 0, 0}; int k = 0, st = 4;
    for (int i = 4; i <= (int)c.length() && k < 5; i++) if (i == (int)c.length() || c[i] == ' ') { if (i > st) a[k++] = c.substring(st, i).toInt(); st = i + 1; }
    searching = false;
    bool f = tryLink(a[0], a[1], a[2], a[3], a[4]);
    if (f) finishFound(); else { scrRx = a[0]; scrTx = a[1]; scrBaud = a[2]; scrInv = a[3]; scrCrc = a[4]; enterBlind(); }
    Serial.println(f ? "SCREEN ANSWERED" : "NO REPLY (blind mode on these settings)");
  }
  else if (c.startsWith("raw ")) {   // raw 5A A5 04 83 00 0F 01
    uint8_t b[64]; int n = 0; String h = c.substring(4); h.replace(" ", "");
    for (int i = 0; i + 1 < (int)h.length() && n < 64; i += 2) b[n++] = strtoul(h.substring(i, i + 2).c_str(), 0, 16);
    drainScreen(); ScreenSerial.write(b, n); ScreenSerial.flush();
    unsigned long t0 = millis(); uint8_t r[64]; int m = 0;
    while (millis() - t0 < 300) { while (ScreenSerial.available() && m < 64) r[m++] = ScreenSerial.read(); delay(1); }
    Serial.println("TX: " + hexs(b, n)); Serial.println(m ? "RX: " + hexs(r, m) : "RX: (nothing)");
  }
  else Serial.println("commands: status | say <text> | clear | detect | ping | log | sniff | cfg rx tx baud inv crc | raw <hex>");
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  handleSerialCmd();

  if (millis() - lastTick >= 250) { lastTick = millis(); tickScheduler(); }

  // keep checking the screen link every 10 s; retry detection every 30 s if lost
  if ((scrFound || scrBlind) && !searching && millis() - lastBeat > 10000) {
    lastBeat = millis();
    uint16_t v;
    if (screenPing(&v, 60)) {
      lastScreenOkMs = millis(); scrVersion = v;
      if (scrBlind) { scrFound = true; scrBlind = false; finishFound(); }
    }
  }
  stepSearch();
  static bool wasLinked = false;
  bool linked = scrFound || scrBlind;
  if (linked && !wasLinked) { screenSetBrightness(brightness); screenSetPage(0); showDefault(); }
  wasLinked = linked;
  // while blind, look again for a real link every 5 minutes
  if (!scrFound && !searching && millis() - searchDoneMs > 300000) startSearch(false);
}

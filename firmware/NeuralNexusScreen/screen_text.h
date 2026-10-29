// Text engine for the DWIN label: UTF-8 -> shaped Arabic + English -> centered 32x5 block.
//  English-only text : 8-bit mode, font 24 (24_ASC.HZK), 20x40 per char      -> 1 byte = 1 column
//  Text with Arabic  : GB2312 mode, ASCII from font 24 (20x40), Arabic glyphs from font 28
//                      (28_AR.HZK, 40x40, stored at GB codes A1A1..)          -> 2 bytes = 2 columns
// Either way the label is 32 columns x 5 rows = 160 bytes.
#pragma once
#include <vector>
#include "arabic_tables.h"

#define TXT_COLS 32
#define TXT_ROWS 5

namespace stext {

typedef std::vector<uint16_t> U16;

inline U16 utf8Decode(const String &s) {
  U16 o; size_t i = 0, n = s.length();
  while (i < n) {
    uint8_t c = s[i];
    uint32_t cp; int k;
    if (c < 0x80) { cp = c; k = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 3; }
    else { i++; continue; }
    bool bad = false;
    for (int j = 1; j <= k; j++) { if (i + j >= n || (s[i + j] & 0xC0) != 0x80) { bad = true; break; } cp = (cp << 6) | (s[i + j] & 0x3F); }
    i += bad ? 1 : k + 1;
    if (bad) continue;
    if (cp == '\r') continue;
    if (cp == '\n' || cp == '\t') cp = ' ';
    if (cp >= 0x064B && cp <= 0x0652) continue;          // harakat: not drawn
    if (cp > 0xFFFF) cp = '?';
    o.push_back((uint16_t)cp);
  }
  return o;
}

inline const ArForm *formOf(uint16_t c) {
  for (int i = 0; i < AR_FORMS_N; i++) if (AR_FORMS[i].base == c) return &AR_FORMS[i];
  return nullptr;
}
inline bool isAr(uint16_t c) { return c >= 0x0621 && c <= 0x064A && formOf(c); }
inline bool dualJoin(uint16_t c) { const ArForm *f = formOf(c); return f && f->ini; }

// logical code points -> presentation forms (still logical order)
inline U16 shape(const U16 &in) {
  U16 o;
  for (size_t i = 0; i < in.size(); i++) {
    uint16_t c = in[i];
    bool prevJoins = i > 0 && isAr(in[i - 1]) && dualJoin(in[i - 1]);
    if (c == 0x0644 && i + 1 < in.size()) {
      uint16_t a = in[i + 1], lig = 0;
      if (a == 0x0622) lig = 0xFEF5; else if (a == 0x0623) lig = 0xFEF7; else if (a == 0x0625) lig = 0xFEF9; else if (a == 0x0627) lig = 0xFEFB;
      if (lig) { o.push_back(lig + (prevJoins ? 1 : 0)); i++; continue; }
    }
    if (isAr(c)) {
      const ArForm *f = formOf(c);
      bool next = f->ini && i + 1 < in.size() && isAr(in[i + 1]);
      uint16_t g = (prevJoins && next) ? f->med : prevJoins ? f->fin : next ? f->ini : f->iso;
      o.push_back(g ? g : f->iso);
    } else o.push_back(c);
  }
  return o;
}

inline bool isLtr(uint16_t c) { return (c < 0x80 && c != ' ') || (c >= 0x0660 && c <= 0x0669); }   // Latin + digits
// one line, logical -> visual (RTL paragraph; Latin/digit runs keep their order)
inline U16 visual(const U16 &seq) {
  std::vector<bool> d(seq.size());
  for (size_t k = 0; k < seq.size(); k++) {
    if (seq[k] != ' ') { d[k] = isLtr(seq[k]); continue; }
    bool a = false, b = false;
    for (int j = (int)k - 1; j >= 0; j--) if (seq[j] != ' ') { a = isLtr(seq[j]); break; }
    for (size_t j = k + 1; j < seq.size(); j++) if (seq[j] != ' ') { b = isLtr(seq[j]); break; }
    d[k] = a && b;
  }
  U16 o; int end = (int)seq.size();
  while (end > 0) {                    // take runs from the end
    int st = end - 1; bool dir = d[st];
    while (st > 0 && d[st - 1] == dir) st--;
    if (dir) for (int k = st; k < end; k++) o.push_back(seq[k]);
    else     for (int k = end - 1; k >= st; k--) o.push_back(seq[k]);
    end = st;
  }
  return o;
}

inline int glyphIdx(uint16_t c) {
  if (c >= 0xFE70 && c <= 0xFEFC) return c - 0xFE70;
  for (int i = 0; i < AR_EXTRA_N; i++) if (AR_EXTRA[i].cp == c) return AR_EXTRA[i].idx;
  return -1;
}
inline int width(uint16_t c, bool ar) { return (ar && c >= 0x80) ? 2 : 1; }

// Build the bytes for the label. arabicMode is set when the GB2312/Arabic font must be used.
inline String build(const String &utf8, bool &arabicMode) {
  U16 cps = utf8Decode(utf8);
  arabicMode = false;
  for (uint16_t c : cps) if ((c >= 0x0600 && c <= 0x06FF)) { arabicMode = true; break; }
  U16 sh;
  if (arabicMode) {
    sh = shape(cps);
    for (auto &c : sh) if (c >= 0x80 && glyphIdx(c) < 0) c = '?';
  } else { sh = cps; for (auto &c : sh) if (c >= 0x80 || c < 0x20) c = '?'; }

  // split into words (logical order)
  std::vector<U16> words; U16 cur;
  for (uint16_t c : sh) { if (c == ' ') { if (cur.size()) { words.push_back(cur); cur.clear(); } } else cur.push_back(c); }
  if (cur.size()) words.push_back(cur);
  auto wlen = [&](const U16 &w) { int n = 0; for (uint16_t c : w) n += width(c, arabicMode); return n; };

  std::vector<U16> lines; U16 line; int lw = 0;
  for (size_t wi = 0; wi < words.size() && (int)lines.size() < TXT_ROWS; wi++) {
    U16 w = words[wi]; int ww = wlen(w);
    while (ww > TXT_COLS) {                      // very long word: hard split
      if (lw) { lines.push_back(line); line.clear(); lw = 0; if ((int)lines.size() >= TXT_ROWS) break; }
      U16 part; int pw = 0; size_t k = 0;
      while (k < w.size() && pw + width(w[k], arabicMode) <= TXT_COLS) { pw += width(w[k], arabicMode); part.push_back(w[k++]); }
      lines.push_back(part); w.erase(w.begin(), w.begin() + k); ww = wlen(w);
      if ((int)lines.size() >= TXT_ROWS) break;
    }
    if ((int)lines.size() >= TXT_ROWS) break;
    if (!w.size()) continue;
    if (!lw) { line = w; lw = ww; }
    else if (lw + 1 + ww <= TXT_COLS) { line.push_back(' '); line.insert(line.end(), w.begin(), w.end()); lw += 1 + ww; }
    else { lines.push_back(line); line = w; lw = ww; }
  }
  if (lw && (int)lines.size() < TXT_ROWS) lines.push_back(line);
  if (!lines.size()) return "";

  String o;
  int top = (TXT_ROWS - (int)lines.size()) / 2;
  for (int k = 0; k < top * TXT_COLS; k++) o += ' ';
  for (size_t li = 0; li < lines.size(); li++) {
    U16 v = arabicMode ? visual(lines[li]) : lines[li];
    int w = 0; for (uint16_t c : v) w += width(c, arabicMode);
    int pad = (TXT_COLS - w) / 2, used = 0;
    for (int k = 0; k < pad; k++) { o += ' '; used++; }
    for (uint16_t c : v) {
      if (c < 0x80) { o += (char)c; used++; }
      else { int g = glyphIdx(c); o += (char)(0xA1 + g / 94); o += (char)(0xA1 + g % 94); used += 2; }
    }
    if (li + 1 < lines.size()) while (used < TXT_COLS) { o += ' '; used++; }
  }
  return o;
}

}  // namespace stext

#include "ui.h"
#include "Arduino_GigaDisplay_GFX.h"

static GigaDisplay_GFX tft;

// RGB565 colours
#define C_BG      0x0000
#define C_HEADER  0x0A5F
#define C_TEXT    0xFFFF
#define C_LABEL   0x9CF3
#define C_OK      0x07E0
#define C_WARN    0xFFE0
#define C_ERR     0xF800
#define C_BAR_BG  0x2945
#define C_BAR     0x05FF

#define W 800
#define H 480
#define ROW_Y0    80
#define ROW_H     46
#define BAR_X     30
#define BAR_Y     320
#define BAR_W     740
#define BAR_H     44
#define RES_Y     408

static int lastPct = -1;

static void text(int x, int y, uint8_t size, uint16_t fg, const char *s) {
  tft.setTextSize(size);
  tft.setTextColor(fg);
  tft.setCursor(x, y);
  tft.print(s);
}

void ui::begin() {
  tft.begin();
  tft.setRotation(1);                 // landscape 800 x 480
  tft.setTextWrap(false);
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, 60, C_HEADER);
  text(20, 16, 4, C_TEXT, "SD -> SWD FLASH PROGRAMMER");
}

void ui::field(uint8_t row, const char *label, const char *value, uint16_t color) {
  int y = ROW_Y0 + row * ROW_H;
  tft.fillRect(0, y - 4, W, ROW_H - 4, C_BG);
  text(30, y + 6, 2, C_LABEL, label);
  char buf[34];                         // 33 chars fit at text size 3
  strncpy(buf, value, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  if (strlen(value) >= sizeof(buf) - 1) strcpy(buf + sizeof(buf) - 4, "...");
  text(190, y, 3, color, buf);
}

static void clearBottom() {
  tft.fillRect(0, BAR_Y - 6, W, H - BAR_Y + 6, C_BG);
  lastPct = -1;
}

void ui::idle(bool sdOk, const char *f405File, const char *g071File) {
  field(ROW_SD, "SD card", sdOk ? "OK" : "NOT FOUND", sdOk ? C_OK : C_ERR);
  field(ROW_FILE, "F405 file", f405File, f405File[0] == '<' ? C_WARN : C_TEXT);
  field(ROW_SIZE, "G071 file", g071File, g071File[0] == '<' ? C_WARN : C_TEXT);
  field(ROW_TARGET, "Target", "-", C_TEXT);
  field(ROW_PHASE, "Status", "READY - waiting for START", C_OK);
  clearBottom();
}

void ui::job(const char *target, const char *file, uint32_t bytes) {
  char b[32];
  snprintf(b, sizeof(b), "%lu bytes (%lu KB)", (unsigned long)bytes, (unsigned long)((bytes + 1023) / 1024));
  field(ROW_SD, "SD card", "OK", C_OK);
  field(ROW_FILE, "File", file, C_TEXT);
  field(ROW_SIZE, "Size", b, C_TEXT);
  field(ROW_TARGET, "Target", target, C_TEXT);
}

void ui::phase(const char *name) {
  field(ROW_PHASE, "Status", name, C_WARN);
  clearBottom();
  tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, C_TEXT);
  tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_BAR_BG);
  progress(0, 1);
}

void ui::progress(uint32_t done, uint32_t total) {
  int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
  if (pct > 100) pct = 100;
  if (pct == lastPct) return;
  int from = lastPct < 0 ? 0 : (BAR_W * lastPct) / 100;
  int to = (BAR_W * pct) / 100;
  if (to > from) tft.fillRect(BAR_X + from, BAR_Y, to - from, BAR_H, C_BAR);
  lastPct = pct;
  char b[8];
  snprintf(b, sizeof(b), "%3d %%", pct);
  tft.fillRect(BAR_X, BAR_Y + BAR_H + 8, 200, 30, C_BG);
  text(BAR_X, BAR_Y + BAR_H + 10, 3, C_TEXT, b);
}

void ui::result(bool ok, const char *msg, uint32_t ms) {
  uint16_t c = ok ? C_OK : C_ERR;
  tft.fillRect(0, RES_Y, W, H - RES_Y, c);
  char b[64];
  if (ok) snprintf(b, sizeof(b), "PASS  (%lu.%lu s)", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
  else    snprintf(b, sizeof(b), "FAIL: %s", msg);
  b[42] = 0;                          // 42 chars fit at text size 3
  text(24, RES_Y + 20, ok ? 4 : 3, C_BG, b);
  field(ROW_PHASE, "Status", ok ? "DONE - ready for next START" : "ERROR - press START to retry",
        ok ? C_OK : C_ERR);
}

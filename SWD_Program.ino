// =============================================================================
//  SWD_Program - stand-alone STM32 flash programmer
//
//  Hardware : Arduino GIGA R1 WiFi + GIGA Display Shield + SPI microSD breakout
//  Targets  : STM32F405RGT6, STM32G071G8U6 (auto-detected over SWD)
//  Interface: SWD on the JTAG pins (SWDIO = TMS/PA13, SWCLK = TCK/PA14) + NRST
//
//  Flow     : START input (active LOW) -> connect -> detect target -> pick the
//             .bin from /F405 or /G071 on the SD card -> erase -> program ->
//             verify -> reset target.  ERROR output goes HIGH on any failure
//             and stays HIGH until the next START.
//
//  Libraries: SdFat (Bill Greiman), Arduino_GigaDisplay_GFX, Adafruit GFX
//  Board    : "Arduino Giga R1" (Arduino Mbed OS Giga Boards core), M7 core
// =============================================================================
// https://github.com/greiman/SdFat

#include <SdFat.h>
#include "config.h"
#include "swd.h"
#include "stm32_flash.h"
#include "ui.h"

SdFs sd;
Swd swd;
Stm32Flash flash(swd);

static uint32_t chunk[1024];               // 4 KB transfer buffer (word aligned)
static char msgBuf[64];
static bool idleScreen = true;
static uint32_t lastScan = 0;

// ---------------------------------------------------------------------------
static bool sdBegin() {
  return sd.begin(SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(SD_SPI_MHZ), &SD_SPI_PORT));
}

// Looks for exactly one *.bin in dir. Returns 1 = found, 0 = none, -1 = several
static int findBin(const char *dir, char *name, size_t len) {
  FsFile d, f;
  int count = 0;
  if (!d.open(dir)) return 0;
  while (f.openNext(&d, O_RDONLY)) {
    char n[96];
    f.getName(n, sizeof(n));
    size_t l = strlen(n);
    if (!f.isDir() && n[0] != '.' && l > 4 && strcasecmp(n + l - 4, ".bin") == 0) {
      if (count == 0) snprintf(name, len, "%s", n);
      count++;
    }
    f.close();
  }
  d.close();
  return count > 1 ? -1 : count;
}

static void describeDir(const char *dir, char *out, size_t len) {
  int r = findBin(dir, out, len);
  if (r == 0) snprintf(out, len, "<no .bin in %s>", dir);
  if (r < 0)  snprintf(out, len, "<several .bin in %s>", dir);
}

static void showIdle(bool force) {
  static char lastA[96], lastB[96];
  static int lastOk = -1;
  char a[96] = "-", b[96] = "-";
  bool ok = sdBegin();
  if (ok) { describeDir(DIR_F405, a, sizeof(a)); describeDir(DIR_G071, b, sizeof(b)); }
  if (!force && ok == lastOk && !strcmp(a, lastA) && !strcmp(b, lastB)) return;  // no flicker
  lastOk = ok; strcpy(lastA, a); strcpy(lastB, b);
  ui::idle(ok, a, b);
}

// debounced, edge triggered: returns true once per activation
static int startStable, startRaw;
static uint32_t startT;
static bool startPressed() {
  int raw = digitalRead(PIN_START);
  if (raw != startRaw) { startRaw = raw; startT = millis(); }
  if (millis() - startT >= START_DEBOUNCE_MS && raw != startStable) {
    startStable = raw;
    return raw == START_ACTIVE;
  }
  return false;
}

static void eraseProgress(uint32_t done, uint32_t total) { ui::progress(done, total); }

// reads the next piece of the file into 'chunk', pads to 8 bytes with 0xFF
static int32_t readChunk(FsFile &f, uint32_t off, uint32_t size, uint32_t &padded) {
  uint32_t n = size - off;
  if (n > sizeof(chunk)) n = sizeof(chunk);
  if ((uint32_t)f.read(chunk, n) != n) return -1;
  padded = (n + 7) & ~7UL;
  memset((uint8_t *)chunk + n, 0xFF, padded - n);
  return n;
}

// ---------------------------------------------------------------------------
// One programming job. Returns nullptr on success, else an error text.
static const char *runJob() {
  if (!sdBegin()) return "SD card not found";

  swd.begin(PIN_SWCLK, PIN_SWDIO, PIN_NRST, SWD_FREQ_HZ);
  ui::job("connecting...", "-", 0);
  ui::phase("Connecting to target (SWD)");
  TargetInfo t;
  if (!flash.attach(t)) return flash.error();

  char tname[48];
  snprintf(tname, sizeof(tname), "%s  %lu KB  rev %04X", t.name,
           (unsigned long)(t.flashBytes / 1024), t.revId);
  Serial.print("Target: "); Serial.println(tname);

  // ---- select the image -------------------------------------------------
  char name[96], path[128];
  int r = findBin(t.dir, name, sizeof(name));
  if (r == 0) { snprintf(msgBuf, sizeof(msgBuf), "No .bin file in %s", t.dir); return msgBuf; }
  if (r < 0)  { snprintf(msgBuf, sizeof(msgBuf), "More than one .bin in %s", t.dir); return msgBuf; }
  snprintf(path, sizeof(path), "%s/%s", t.dir, name);

  FsFile f;
  if (!f.open(path, O_RDONLY)) return "Cannot open file";
  uint32_t size = f.fileSize();
  ui::job(tname, name, size);
  Serial.print("File: "); Serial.print(path); Serial.print("  "); Serial.println(size);

  if (size < 8) return "File is empty";
  if (size > t.flashBytes) return "File larger than target flash";

  // sanity check: Cortex-M vector table (initial SP in SRAM, reset vector in flash)
  uint32_t vt[2];
  if (f.read(vt, 8) != 8) return "SD read error";
  if ((vt[0] & 0xFF000000UL) != 0x20000000UL ||
      vt[1] < STM32_FLASH_BASE || vt[1] >= STM32_FLASH_BASE + size)
    return "Not an STM32 image (vector table)";

  // ---- erase --------------------------------------------------------------
  ui::phase("1/3  ERASING");
  if (!flash.erase(size, eraseProgress)) return flash.error();

  // ---- program ------------------------------------------------------------
  ui::phase("2/3  PROGRAMMING");
  f.seekSet(0);
  for (uint32_t off = 0; off < size;) {
    uint32_t padded;
    int32_t n = readChunk(f, off, size, padded);
    if (n < 0) return "SD read error";
    if (!flash.program(off, (const uint8_t *)chunk, padded)) return flash.error();
    off += n;
    ui::progress(off, size);
  }

  // ---- verify -------------------------------------------------------------
#if VERIFY_AFTER_PROGRAM
  ui::phase("3/3  VERIFYING");
  f.seekSet(0);
  for (uint32_t off = 0; off < size;) {
    uint32_t padded;
    int32_t n = readChunk(f, off, size, padded);
    if (n < 0) return "SD read error";
    if (!flash.verify(off, (const uint8_t *)chunk, padded)) return flash.error();
    off += n;
    ui::progress(off, size);
  }
#endif
  f.close();
  return nullptr;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_ERROR, OUTPUT); digitalWrite(PIN_ERROR, LOW);
  pinMode(PIN_BUSY, OUTPUT);  digitalWrite(PIN_BUSY, LOW);
  pinMode(PIN_PASS, OUTPUT);  digitalWrite(PIN_PASS, LOW);
  startRaw = startStable = digitalRead(PIN_START);   // held at boot != start
  startT = millis();

  swd.begin(PIN_SWCLK, PIN_SWDIO, PIN_NRST, SWD_FREQ_HZ);
  swd.end();                                         // lines high-Z while idle

  ui::begin();
  showIdle(true);
  lastScan = millis();
}

void loop() {
  if (startPressed()) {
    idleScreen = false;
    digitalWrite(PIN_ERROR, LOW);
    digitalWrite(PIN_PASS, LOW);
    digitalWrite(PIN_BUSY, HIGH);
    Serial.println("--- START ---");

    uint32_t t0 = millis();
    const char *err = runJob();
    bool ok = (err == nullptr);
    flash.finish(ok && RUN_TARGET_AFTER_PASS);
    uint32_t ms = millis() - t0;

    digitalWrite(PIN_BUSY, LOW);
    digitalWrite(ok ? PIN_PASS : PIN_ERROR, HIGH);
    ui::result(ok, ok ? "" : err, ms);
    Serial.println(ok ? "PASS" : err);
  }

  // before the first job: refresh the file list (card swapped etc.)
  if (idleScreen && millis() - lastScan > 3000) {
    showIdle(false);
    lastScan = millis();
  }
}

// =============================================================================
//  STM32F405 / STM32G071 flash programming over SWD
//  References: RM0090 (STM32F405/407), RM0444 (STM32G0x1), ARMv7-M / ARMv6-M ARM
// =============================================================================
#include "stm32_flash.h"
#include "config.h"

// ---- Cortex-M debug registers ----------------------------------------------
#define CPUID        0xE000ED00UL
#define AIRCR        0xE000ED0CUL
#define DHCSR        0xE000EDF0UL
#define DEMCR        0xE000EDFCUL
#define DBGKEY       0xA05F0000UL
#define C_DEBUGEN    (1UL << 0)
#define C_HALT       (1UL << 1)
#define S_HALT       (1UL << 17)
#define VC_CORERESET (1UL << 0)
#define AIRCR_RESET  0x05FA0004UL     // VECTKEY | SYSRESETREQ

#define STM_KEY1   0x45670123UL
#define STM_KEY2   0xCDEF89ABUL
#define CR_LOCK      (1UL << 31)
#define CR_STRT      (1UL << 16)
#define CR_PG        (1UL << 0)

// ---- STM32F405 (RM0090) ----------------------------------------------------
#define F4_IDCODE    0xE0042000UL
#define F4_FLASHSIZE 0x1FFF7A20UL     // upper 16 bits of this word = size in KB
#define F4_KEYR      0x40023C04UL
#define F4_SR        0x40023C0CUL
#define F4_CR        0x40023C10UL
#define F4_OPTCR     0x40023C14UL
#define F4_CR_SER    (1UL << 1)
#define F4_PSIZE_X32 (2UL << 8)       // needs VDD 2.7 .. 3.6 V
#define F4_SR_BSY    (1UL << 16)
#define F4_SR_ERR    0x000001F2UL     // RDERR|PGSERR|PGPERR|PGAERR|WRPERR|OPERR

// ---- STM32G071 (RM0444) ----------------------------------------------------
#define G0_IDCODE    0x40015800UL
#define G0_FLASHSIZE 0x1FFF75E0UL     // lower 16 bits = size in KB
#define G0_KEYR      0x40022008UL
#define G0_SR        0x40022010UL
#define G0_CR        0x40022014UL
#define G0_OPTR      0x40022020UL
#define G0_CR_PER    (1UL << 1)
#define G0_PAGE      2048UL
#define G0_SR_BSY    ((1UL << 16) | (1UL << 18))   // BSY1 | CFGBSY
#define G0_SR_ERR    0x0000C3FAUL     // OPTV|RD|FAST|MISS|PGS|SIZ|PGA|WRP|PROG|OP

#define F4_REG(f4, g0) ((_t.family == TGT_F4) ? (f4) : (g0))

// ----------------------------------------------------------------------------
bool Stm32Flash::attach(TargetInfo &info) {
  info = TargetInfo();
  _t = info;
  uint32_t dpidr = 0;
  bool underReset = false;

  if (!_swd.connect(dpidr)) {
    // firmware may have disabled SWD pins or sleeps -> connect under reset
    if (!_swd.hasReset()) return swdFail();
    _swd.setReset(true);
    delay(20);
    if (!_swd.connect(dpidr)) { _swd.setReset(false); return swdFail(); }
    underReset = true;
  }
  info.dpidr = dpidr;

  // Halt the core and catch it at the reset vector -> clean peripheral state
  if (!_swd.write32(DHCSR, DBGKEY | C_HALT | C_DEBUGEN)) return swdFail();
  if (!_swd.write32(DEMCR, VC_CORERESET)) return swdFail();
  if (underReset) _swd.setReset(false);
  else            _swd.write32(AIRCR, AIRCR_RESET);   // ack may be lost in reset
  delay(50);
  if (!_swd.connect(dpidr)) return swdFail();

  uint32_t dh = 0, t0 = millis();
  do {
    if (!_swd.read32(DHCSR, dh)) return swdFail();
    if (dh & S_HALT) break;
  } while (millis() - t0 < 200);
  if (!(dh & S_HALT)) {
    _swd.write32(DHCSR, DBGKEY | C_HALT | C_DEBUGEN);
    delay(5);
    if (!_swd.read32(DHCSR, dh) || !(dh & S_HALT)) return fail("Cannot halt target core");
  }

  // ---- identify ----
  uint32_t cpuid, idc, fs = 0;
  if (!_swd.read32(CPUID, cpuid)) return swdFail();
  uint16_t part = (cpuid >> 4) & 0xFFF;
  if      (part == 0xC24) { if (!_swd.read32(F4_IDCODE, idc)) return swdFail(); }   // Cortex-M4
  else if (part == 0xC60) { if (!_swd.read32(G0_IDCODE, idc)) return swdFail(); }   // Cortex-M0+
  else {
    snprintf(_errBuf, sizeof(_errBuf), "Unsupported core (CPUID 0x%08lX)", (unsigned long)cpuid);
    return fail(_errBuf);
  }
  info.devId = idc & 0xFFF;
  info.revId = idc >> 16;

  uint32_t rdp;
  if (part == 0xC24 && info.devId == 0x413) {
    info.family = TGT_F4; info.name = "STM32F405/407"; info.dir = DIR_F405;
    if (!_swd.read32(F4_OPTCR, rdp)) return swdFail();
    rdp = (rdp >> 8) & 0xFF;
    if (_swd.read32(F4_FLASHSIZE, fs)) fs >>= 16;
    if (fs == 0 || fs == 0xFFFF) fs = 1024;
  } else if (part == 0xC60 && info.devId == 0x460) {
    info.family = TGT_G0; info.name = "STM32G071/081"; info.dir = DIR_G071;
    if (!_swd.read32(G0_OPTR, rdp)) return swdFail();
    rdp &= 0xFF;
    if (_swd.read32(G0_FLASHSIZE, fs)) fs &= 0xFFFF;
    if (fs == 0 || fs == 0xFFFF) fs = 64;
  } else {
    snprintf(_errBuf, sizeof(_errBuf), "Unsupported device ID 0x%03X", info.devId);
    return fail(_errBuf);
  }
  info.flashBytes = fs * 1024UL;
  _t = info;

  if (rdp != 0xAA) return fail("Read-out protection (RDP) active");
  return true;
}

// ----------------------------------------------------------------------------
bool Stm32Flash::waitIdle(uint32_t timeoutMs) {
  uint32_t sr, busy = F4_REG(F4_SR_BSY, G0_SR_BSY), t0 = millis();
  for (;;) {
    if (!_swd.read32(F4_REG(F4_SR, G0_SR), sr)) return swdFail();
    if (!(sr & busy)) return true;
    if (millis() - t0 > timeoutMs) return fail("Flash busy timeout");
  }
}

bool Stm32Flash::checkErrors() {
  uint32_t sr, mask = F4_REG(F4_SR_ERR, G0_SR_ERR);
  if (!_swd.read32(F4_REG(F4_SR, G0_SR), sr)) return swdFail();
  if (!(sr & mask)) return true;
  _swd.write32(F4_REG(F4_SR, G0_SR), sr & mask);         // clear (write 1)
  if (sr & (1UL << 4)) return fail("Flash write-protected (WRP)");
  snprintf(_errBuf, sizeof(_errBuf), "Flash error, SR=0x%08lX", (unsigned long)sr);
  return fail(_errBuf);
}

bool Stm32Flash::unlock() {
  uint32_t cr, crAddr = F4_REG(F4_CR, G0_CR), keyr = F4_REG(F4_KEYR, G0_KEYR);
  if (!_swd.read32(crAddr, cr)) return swdFail();
  if (cr & CR_LOCK) {
    if (!_swd.write32(keyr, STM_KEY1) || !_swd.write32(keyr, STM_KEY2)) return swdFail();
    if (!_swd.read32(crAddr, cr)) return swdFail();
    if (cr & CR_LOCK) return fail("Flash unlock failed");
  }
  return true;
}

// ----------------------------------------------------------------------------
static uint32_t f4SectorStart(uint32_t s) {
  if (s < 4)  return s * 0x4000UL;              // 4 x 16 KB
  if (s == 4) return 0x10000UL;                 // 1 x 64 KB
  return (s - 4) * 0x20000UL;                   // 7 x 128 KB
}

bool Stm32Flash::erase(uint32_t imageBytes, ProgressFn cb) {
  if (imageBytes == 0 || imageBytes > _t.flashBytes) return fail("Image larger than target flash");
  if (!unlock() || !waitIdle(500)) return false;
  uint32_t srAddr = F4_REG(F4_SR, G0_SR), crAddr = F4_REG(F4_CR, G0_CR);
  _swd.write32(srAddr, F4_REG(F4_SR_ERR, G0_SR_ERR));     // clear stale flags

  if (_t.family == TGT_F4) {
    uint32_t n = 0;
    while (n < 12 && f4SectorStart(n) < imageBytes) n++;
    for (uint32_t s = 0; s < n; s++) {
      uint32_t cr = F4_PSIZE_X32 | F4_CR_SER | (s << 3);
      if (!_swd.write32(crAddr, cr) || !_swd.write32(crAddr, cr | CR_STRT)) return swdFail();
      if (!waitIdle(10000) || !checkErrors()) return false;   // 128 KB: up to ~4 s
      if (cb) cb(s + 1, n);
    }
  } else {
    uint32_t n = (imageBytes + G0_PAGE - 1) / G0_PAGE;
    for (uint32_t p = 0; p < n; p++) {
      uint32_t cr = G0_CR_PER | (p << 3);
      if (!_swd.write32(crAddr, cr) || !_swd.write32(crAddr, cr | CR_STRT)) return swdFail();
      if (!waitIdle(1000) || !checkErrors()) return false;
      if (cb) cb(p + 1, n);
    }
  }
  return _swd.write32(crAddr, 0) ? true : swdFail();
}

// data must be 4-byte aligned, len a multiple of 8 (pad with 0xFF)
bool Stm32Flash::program(uint32_t offset, const uint8_t *data, uint32_t len) {
  const uint32_t *w = (const uint32_t *)data;
  uint32_t words = len / 4, addr = STM32_FLASH_BASE + offset;
  uint32_t crAddr = F4_REG(F4_CR, G0_CR);
  if (!waitIdle(500)) return false;

  if (_t.family == TGT_F4) {
    // 32-bit parallelism; AHB stalls (SWD WAIT) while a word is being written
    if (!_swd.write32(crAddr, F4_PSIZE_X32 | CR_PG)) return swdFail();
    while (words) {
      uint32_t n = words > 256 ? 256 : words;
      if (!_swd.writeBlock(addr, w, n)) return swdFail();
      if (!waitIdle(100) || !checkErrors()) return false;
      addr += n * 4; w += n; words -= n;
    }
  } else {
    // G0: 64-bit (double word) programming, wait after each double word
    if (!_swd.write32(crAddr, CR_PG)) return swdFail();
    for (uint32_t i = 0; i < words; i += 2) {
      if (!_swd.writeBlock(addr, w + i, 2)) return swdFail();
      if (!waitIdle(50)) return false;
      addr += 8;
    }
    if (!checkErrors()) return false;
  }
  return _swd.write32(crAddr, 0) ? true : swdFail();
}

bool Stm32Flash::verify(uint32_t offset, const uint8_t *data, uint32_t len) {
  static uint32_t rb[256];
  const uint32_t *w = (const uint32_t *)data;
  uint32_t words = len / 4, addr = STM32_FLASH_BASE + offset;
  while (words) {
    uint32_t n = words > 256 ? 256 : words;
    if (!_swd.readBlock(addr, rb, n)) return swdFail();
    for (uint32_t i = 0; i < n; i++) {
      if (rb[i] != w[i]) {
        snprintf(_errBuf, sizeof(_errBuf), "Verify failed @0x%08lX", (unsigned long)(addr + i * 4));
        return fail(_errBuf);
      }
    }
    addr += n * 4; w += n; words -= n;
  }
  return true;
}

// ----------------------------------------------------------------------------
void Stm32Flash::finish(bool runTarget) {
  if (_t.family != TGT_NONE)
    _swd.write32(F4_REG(F4_CR, G0_CR), CR_LOCK);        // re-lock flash
  if (runTarget) {
    _swd.write32(DEMCR, 0);                              // no vector catch
    _swd.write32(DHCSR, DBGKEY);                         // leave debug, run
    _swd.write32(AIRCR, AIRCR_RESET);
    delay(10);
    _swd.clearErrors();
    _swd.writeDP(DP_CTRLSTAT, 0);                        // power down debug domain
    if (_swd.hasReset()) { _swd.setReset(true); delay(20); _swd.setReset(false); }
  }
  _swd.end();
}

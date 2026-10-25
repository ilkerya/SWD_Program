// =============================================================================
//  Bit-banged SWD host for the Arduino GIGA R1 (STM32H747, Cortex-M7 @ 480 MHz)
//  GPIOs are driven through their registers for speed; the bit period is timed
//  with the DWT cycle counter so the SWD clock is independent of code changes.
// =============================================================================
#include "swd.h"
#include "pinDefinitions.h"

extern "C" uint32_t SystemCoreClock;

// ---- raw register addresses (STM32H7) ---------------------------------------
#define H7_GPIO_BASE   0x58020000UL   // GPIOA, ports spaced 0x400
#define H7GPIO_MODER     0x00
#define H7GPIO_OSPEEDR   0x08
#define H7GPIO_PUPDR     0x0C
#define H7GPIO_IDR       0x10
#define H7GPIO_BSRR      0x18
#define REG(a)         (*(volatile uint32_t *)(a))

#define H7_DEMCR       0xE000EDFCUL
#define H7DWT_CTRL       0xE0001000UL
#define H7DWT_CYCCNT     0xE0001004UL
#define H7DWT_LAR        0xE0001FB0UL

#define WAIT_RETRIES   2000

static uint32_t gpioBase(int arduinoPin, uint32_t &pin) {
  uint32_t pn = (uint32_t)digitalPinToPinName(arduinoPin);
  pin = pn & 0x0F;
  return H7_GPIO_BASE + 0x400UL * ((pn >> 4) & 0x0F);
}

// ----------------------------------------------------------------------------
void Swd::begin(int swclk, int swdio, int nrst, uint32_t freqHz) {
  _nrst = nrst;

  // cycle counter for precise bit timing
  REG(H7_DEMCR) |= (1UL << 24);       // TRCENA
  REG(H7DWT_LAR) = 0xC5ACCE55;          // unlock (Cortex-M7)
  REG(H7DWT_CYCCNT) = 0;
  REG(H7DWT_CTRL) |= 1UL;               // CYCCNTENA
  _half = SystemCoreClock / (2UL * freqHz);
  if (_half < 4) _half = 4;

  // let the core set up clocks/modes, then take over the registers
  pinMode(swclk, OUTPUT);
  pinMode(swdio, OUTPUT);
  uint32_t ckPin, ioPin;
  uint32_t ckBase = gpioBase(swclk, ckPin);
  uint32_t ioBase = gpioBase(swdio, ioPin);
  _ckBsrr  = (volatile uint32_t *)(ckBase + H7GPIO_BSRR);
  _ioBsrr  = (volatile uint32_t *)(ioBase + H7GPIO_BSRR);
  _ioIdr   = (volatile uint32_t *)(ioBase + H7GPIO_IDR);
  _ioModer = (volatile uint32_t *)(ioBase + H7GPIO_MODER);
  _ckMask = 1UL << ckPin;
  _ioMask = 1UL << ioPin;
  _ioPin  = ioPin;

  // high speed outputs, pull-up on SWDIO
  REG(ckBase + H7GPIO_OSPEEDR) |= (3UL << (2 * ckPin));
  REG(ioBase + H7GPIO_OSPEEDR) |= (3UL << (2 * ioPin));
  REG(ioBase + H7GPIO_PUPDR) = (REG(ioBase + H7GPIO_PUPDR) & ~(3UL << (2 * ioPin))) | (1UL << (2 * ioPin));

  *_ckBsrr = _ckMask;                 // SWCLK idles high
  *_ioBsrr = _ioMask;
  setReset(false);
  _select = 0xFFFFFFFF;
}

void Swd::end() {
  dirIn();
  setReset(false);
}

void Swd::setReset(bool asserted) {
  if (_nrst < 0) return;
  if (asserted) { pinMode(_nrst, OUTPUT); digitalWrite(_nrst, LOW); }
  else          { pinMode(_nrst, INPUT_PULLUP); }   // open-drain style release
}

// ---- low level --------------------------------------------------------------
inline void Swd::waitHalf() {
  uint32_t s = REG(H7DWT_CYCCNT);
  while ((REG(H7DWT_CYCCNT) - s) < _half) { }
}
inline void Swd::dirOut() {
  *_ioModer = (*_ioModer & ~(3UL << (2 * _ioPin))) | (1UL << (2 * _ioPin));
}
inline void Swd::dirIn() {
  *_ioModer &= ~(3UL << (2 * _ioPin));
}

void Swd::writeBits(uint32_t v, uint8_t n) {
  while (n--) {
    *_ioBsrr = (v & 1) ? _ioMask : (_ioMask << 16);
    *_ckBsrr = _ckMask << 16;         // SWCLK low
    waitHalf();
    *_ckBsrr = _ckMask;               // SWCLK high -> target samples
    waitHalf();
    v >>= 1;
  }
}

uint32_t Swd::readBits(uint8_t n) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < n; i++) {
    *_ckBsrr = _ckMask << 16;
    waitHalf();
    if (*_ioIdr & _ioMask) v |= (1UL << i);
    *_ckBsrr = _ckMask;
    waitHalf();
  }
  return v;
}

void Swd::lineReset() {
  dirOut();
  writeBits(0xFFFFFFFF, 32);
  writeBits(0xFFFFFFFF, 28);          // >= 50 cycles with SWDIO high
}

// One SWD transaction. req: bit0 APnDP, bit1 RnW, bits[3:2] = A[3:2]
uint8_t Swd::transfer(uint8_t req, uint32_t *data) {
  req &= 0x0F;
  uint8_t hdr = 0x81 | (req << 1) | ((__builtin_popcount(req) & 1) << 5);
  bool rd = req & 0x02;

  for (int retry = 0; retry < WAIT_RETRIES; retry++) {
    dirOut();
    writeBits(hdr, 8);
    dirIn();
    readBits(1);                      // turnaround
    uint8_t ack = readBits(3);

    if (ack == SWD_ACK_OK) {
      if (rd) {
        uint32_t v = readBits(32);
        uint32_t p = readBits(1);
        readBits(1);                  // turnaround
        dirOut();
        writeBits(0, 2);              // idle
        if ((__builtin_popcount(v) & 1) != (int)p) return SWD_ERR_PARITY;
        *data = v;
      } else {
        readBits(1);                  // turnaround
        dirOut();
        writeBits(*data, 32);
        writeBits(__builtin_popcount(*data) & 1, 1);
        writeBits(0, 2);              // idle
      }
      return SWD_ACK_OK;
    }

    if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
      readBits(1);                    // turnaround back to host
      dirOut();
      writeBits(0, 2);
      if (ack == SWD_ACK_FAULT) return ack;
      continue;                       // WAIT -> retry
    }

    // no / invalid response: clock out a full data phase and give up
    readBits(33);
    readBits(1);
    dirOut();
    writeBits(0, 2);
    return ack;
  }
  return SWD_ACK_WAIT;
}

bool Swd::check(uint8_t ack, const char *what) {
  if (ack == SWD_ACK_OK) return true;
  switch (ack) {
    case SWD_ACK_WAIT:   _err = "SWD timeout (WAIT)"; break;
    case SWD_ACK_FAULT:  _err = "SWD FAULT";          break;
    case SWD_ERR_PARITY: _err = "SWD parity error";   break;
    default:             _err = "No SWD response";    break;
  }
  (void)what;
  if (ack == SWD_ACK_FAULT) clearErrors();
  return false;
}

// ---- DP / AP ------------------------------------------------------------------
bool Swd::readDP(uint8_t addr, uint32_t &v)  { return check(transfer(0x02 | (addr & 0x0C), &v), "rdDP"); }
bool Swd::writeDP(uint8_t addr, uint32_t v)  { return check(transfer(0x00 | (addr & 0x0C), &v), "wrDP"); }

bool Swd::readAP(uint8_t addr, uint32_t &v) {
  uint32_t sel = addr & 0xF0;         // AP #0, bank
  if (sel != _select) { if (!writeDP(DP_SELECT, sel)) return false; _select = sel; }
  return check(transfer(0x03 | (addr & 0x0C), &v), "rdAP");
}
bool Swd::writeAP(uint8_t addr, uint32_t v) {
  uint32_t sel = addr & 0xF0;
  if (sel != _select) { if (!writeDP(DP_SELECT, sel)) return false; _select = sel; }
  return check(transfer(0x01 | (addr & 0x0C), &v), "wrAP");
}

void Swd::clearErrors() {
  uint32_t v = 0x1E;                  // STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR
  transfer(0x00, &v);
}

bool Swd::connect(uint32_t &dpidr) {
  _select = 0xFFFFFFFF;
  lineReset();
  writeBits(0xE79E, 16);              // JTAG-to-SWD switch sequence
  lineReset();
  writeBits(0, 8);                    // idle cycles
  if (!readDP(DP_IDCODE, dpidr)) return false;   // mandatory first read

  clearErrors();
  if (!writeDP(DP_SELECT, 0)) return false;
  _select = 0;
  if (!writeDP(DP_CTRLSTAT, 0x50000000)) return false;   // CSYSPWRUPREQ|CDBGPWRUPREQ
  uint32_t t0 = millis(), cs = 0;
  do {
    if (!readDP(DP_CTRLSTAT, cs)) return false;
    if ((cs & 0xA0000000) == 0xA0000000) break;
  } while (millis() - t0 < 100);
  if ((cs & 0xA0000000) != 0xA0000000) { _err = "Debug power-up failed"; return false; }

  // 32-bit accesses, auto-increment, debug master
  return writeAP(AP_CSW, 0x23000052);
}

// ---- memory -------------------------------------------------------------------
bool Swd::read32(uint32_t addr, uint32_t &v) {
  uint32_t dummy;
  return writeAP(AP_TAR, addr) && readAP(AP_DRW, dummy) && readDP(DP_RDBUFF, v);
}

bool Swd::write32(uint32_t addr, uint32_t v) {
  uint32_t dummy;
  return writeAP(AP_TAR, addr) && writeAP(AP_DRW, v) && readDP(DP_RDBUFF, dummy);
}

// TAR auto-increment is only guaranteed inside a 1 KB block -> re-load TAR there
bool Swd::writeBlock(uint32_t addr, const uint32_t *buf, uint32_t words) {
  while (words) {
    uint32_t n = (0x400 - (addr & 0x3FF)) >> 2;
    if (n > words) n = words;
    if (!writeAP(AP_TAR, addr)) return false;
    for (uint32_t i = 0; i < n; i++)
      if (!writeAP(AP_DRW, buf[i])) return false;
    addr += n << 2; buf += n; words -= n;
  }
  uint32_t dummy;
  return readDP(DP_RDBUFF, dummy);    // flush posted write, catch faults
}

bool Swd::readBlock(uint32_t addr, uint32_t *buf, uint32_t words) {
  while (words) {
    uint32_t n = (0x400 - (addr & 0x3FF)) >> 2;
    if (n > words) n = words;
    uint32_t dummy;
    if (!writeAP(AP_TAR, addr)) return false;
    if (!readAP(AP_DRW, dummy)) return false;          // starts the pipeline
    for (uint32_t i = 0; i + 1 < n; i++)
      if (!readAP(AP_DRW, buf[i])) return false;
    if (!readDP(DP_RDBUFF, buf[n - 1])) return false;  // last word
    addr += n << 2; buf += n; words -= n;
  }
  return true;
}

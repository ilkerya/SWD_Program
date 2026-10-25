// =============================================================================
//  Bit-banged ARM Serial Wire Debug (SWD) host + MEM-AP access
// =============================================================================
#pragma once
#include <Arduino.h>

// DP register addresses
#define DP_IDCODE    0x00  // read
#define DP_ABORT     0x00  // write
#define DP_CTRLSTAT  0x04
#define DP_SELECT    0x08  // write
#define DP_RDBUFF    0x0C  // read

// MEM-AP register addresses (bank in bits [7:4])
#define AP_CSW       0x00
#define AP_TAR       0x04
#define AP_DRW       0x0C
#define AP_IDR       0xFC

// SWD ACK values
#define SWD_ACK_OK     0x1
#define SWD_ACK_WAIT   0x2
#define SWD_ACK_FAULT  0x4
#define SWD_ERR_PARITY 0x8

class Swd {
public:
  void begin(int swclk, int swdio, int nrst, uint32_t freqHz);
  void end();                         // release all lines (high-Z)
  void setReset(bool asserted);       // drive NRST low / release
  bool hasReset() const { return _nrst >= 0; }

  bool connect(uint32_t &dpidr);      // line reset, JTAG->SWD, power-up debug
  bool readDP(uint8_t addr, uint32_t &v);
  bool writeDP(uint8_t addr, uint32_t v);
  bool readAP(uint8_t addr, uint32_t &v);   // posted read (result of previous)
  bool writeAP(uint8_t addr, uint32_t v);
  void clearErrors();

  // Target memory (32-bit, word aligned)
  bool read32(uint32_t addr, uint32_t &v);
  bool write32(uint32_t addr, uint32_t v);
  bool readBlock(uint32_t addr, uint32_t *buf, uint32_t words);
  bool writeBlock(uint32_t addr, const uint32_t *buf, uint32_t words);

  const char *lastError() const { return _err; }

private:
  uint8_t transfer(uint8_t req, uint32_t *data);
  bool    check(uint8_t ack, const char *what);
  void    lineReset();
  void    writeBits(uint32_t v, uint8_t n);
  uint32_t readBits(uint8_t n);
  void    dirOut();
  void    dirIn();
  void    waitHalf();

  int      _nrst = -1;
  uint32_t _half = 240;               // CPU cycles per half clock period
  uint32_t _select = 0xFFFFFFFF;      // cached DP SELECT
  const char *_err = "";

  // fast GPIO
  volatile uint32_t *_ckBsrr, *_ioBsrr, *_ioIdr, *_ioModer;
  uint32_t _ckMask, _ioMask, _ioPin;
};

// =============================================================================
//  STM32 flash programming over SWD (host driven, no RAM loader)
//  Supported: STM32F405/407 (dev 0x413) and STM32G071/081 (dev 0x460)
// =============================================================================
#pragma once
#include "swd.h"

#define STM32_FLASH_BASE 0x08000000UL

enum TargetFamily { TGT_NONE = 0, TGT_F4, TGT_G0 };

struct TargetInfo {
  TargetFamily family = TGT_NONE;
  uint16_t devId = 0;
  uint16_t revId = 0;
  uint32_t flashBytes = 0;
  uint32_t dpidr = 0;
  const char *name = "-";
  const char *dir = "";
};

typedef void (*ProgressFn)(uint32_t done, uint32_t total);

class Stm32Flash {
public:
  explicit Stm32Flash(Swd &swd) : _swd(swd) {}

  bool attach(TargetInfo &info);                       // connect, reset-halt, identify
  bool erase(uint32_t imageBytes, ProgressFn cb);      // erase sectors/pages used
  bool program(uint32_t offset, const uint8_t *data, uint32_t len);  // len % 8 == 0
  bool verify(uint32_t offset, const uint8_t *data, uint32_t len);
  void finish(bool runTarget);                         // lock, release, reset

  const char *error() const { return _err; }

private:
  bool unlock();
  bool waitIdle(uint32_t timeoutMs);
  bool checkErrors();
  bool fail(const char *msg) { _err = msg; return false; }
  bool swdFail() { _err = _swd.lastError(); return false; }

  Swd &_swd;
  TargetInfo _t;
  const char *_err = "";
  char _errBuf[64];
};

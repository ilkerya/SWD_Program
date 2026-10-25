// =============================================================================
//  User interface on the GIGA Display Shield (800 x 480, landscape)
// =============================================================================
#pragma once
#include <Arduino.h>

namespace ui {
  void begin();
  void idle(bool sdOk, const char *f405File, const char *g071File);
  void job(const char *target, const char *file, uint32_t bytes);
  void field(uint8_t row, const char *label, const char *value, uint16_t color);
  void phase(const char *name);            // resets the progress bar
  void progress(uint32_t done, uint32_t total);
  void result(bool ok, const char *msg, uint32_t ms);

  // rows used with field()
  enum { ROW_SD = 0, ROW_FILE, ROW_SIZE, ROW_TARGET, ROW_PHASE };
}

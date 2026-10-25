// =============================================================================
//  SWD_Program - configuration
//  Arduino GIGA R1 WiFi + GIGA Display Shield + SPI microSD breakout
//  Targets: STM32F405RGT6 (Cortex-M4) and STM32G071G8U6 (Cortex-M0+)
// =============================================================================
#pragma once

// ---------------- microSD (SPI1 = header pins D10..D13) ----------------------
//  D10 = CS, D11 = MOSI (COPI), D12 = MISO (CIPO), D13 = SCK
//  If you prefer the 6-pin ICSP/SPI header instead, use SPI (pins 89/90/91)
#define SD_CS_PIN        10
#define SD_SPI_PORT      SPI1
#define SD_SPI_MHZ       16          // lower to 4 if you get SD read errors

// ---------------- SWD target interface ---------------------------------------
//  The JTAG connector's TMS/TCK pins are used as SWDIO/SWCLK.
//  (STM32G0 has NO JTAG, only SWD - SWD works for both targets.)
#define PIN_SWCLK        22          // -> target PA14 (SWCLK / JTCK)
#define PIN_SWDIO        23          // -> target PA13 (SWDIO / JTMS)
#define PIN_NRST         24          // -> target NRST (open-drain emulation), -1 = unused
#define SWD_FREQ_HZ      1000000UL   // 1 MHz is safe for ~20 cm flying leads; max ~4 MHz

// ---------------- Control I/O ------------------------------------------------
#define PIN_START        2           // start input, internal pull-up
#define START_ACTIVE     LOW         // LOW = button/open-collector to GND starts a job
#define START_DEBOUNCE_MS 30

#define PIN_ERROR        3           // driven HIGH on any error (until next start)
#define PIN_BUSY         4           // HIGH while a job is running   (optional)
#define PIN_PASS         5           // HIGH after a successful job   (optional)

// ---------------- Behaviour --------------------------------------------------
#define VERIFY_AFTER_PROGRAM  1      // read back and compare with the file
#define RUN_TARGET_AFTER_PASS 1      // reset & start target after success

// One folder per target type on the SD card, each holding exactly ONE .bin
// (raw binary, linked for 0x08000000). The programmer auto-detects the target
// over SWD and picks the file from the matching folder.
#define DIR_F405         "/F405"
#define DIR_G071         "/G071"

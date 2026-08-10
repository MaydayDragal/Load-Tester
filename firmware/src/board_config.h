// Board selector.
//
// This firmware targets two Waveshare boards. Exactly one board macro is
// defined by the PlatformIO env (`-D BOARD_C6_AMOLED` / `-D BOARD_S3_LCD35B` in
// platformio.ini); this header picks the matching pin/capability map. Every
// other source file includes THIS header and nothing board-specific.
//
//   BOARD_C6_AMOLED  ESP32-C6-Touch-AMOLED-1.8 — 368x448 QSPI AMOLED, no PSRAM,
//                    bit-banged software-SPI SD. **Hardware-verified**: this is
//                    the board every bench result in HANDOVER.md came from.
//   BOARD_S3_LCD35B  ESP32-S3-Touch-LCD-3.5B — 320x480 QSPI AXS15231B IPS, 8 MB
//                    PSRAM, hardware SDMMC. Boot, PSRAM and the I2C bus are
//                    hardware-confirmed; the rest is being brought up. (The "-C"
//                    SKU is this board plus a camera; identical otherwise.)
//
// This target was written for the ESP32-S3-Touch-LCD-3.5 (no B) until 2026-08-10
// and repointed when the hardware arrived and turned out to be the 3.5B. The two
// are different products — different panel controller, different transport, four
// conflicting GPIOs — so a 3.5 is NOT supported by this build. git history has
// the old board_s3_lcd35.h if one ever needs to come back.
//
// Each board header defines the same set of names, so the rest of the codebase
// is board-agnostic. The capability flags (BOARD_PANEL_*, BOARD_TOUCH_*,
// BOARD_SD_*, BOARD_BACKLIGHT_PWM, BOARD_HAS_PSRAM) are what display.cpp and
// sd_card.cpp switch on — prefer adding a capability flag over testing the board
// macro directly, so a third board stays a matter of writing one header.
#pragma once

#if defined(BOARD_C6_AMOLED) && defined(BOARD_S3_LCD35B)
#error "Define exactly ONE board macro (BOARD_C6_AMOLED or BOARD_S3_LCD35B)"
#elif defined(BOARD_C6_AMOLED)
#include "board_c6_amoled.h"
#elif defined(BOARD_S3_LCD35B)
#include "board_s3_lcd35b.h"
#elif defined(BOARD_S3_LCD35)
// Caught deliberately rather than falling through to "no board selected", so an
// old env or a stale build flag names the actual problem.
#error "BOARD_S3_LCD35 (the plain 3.5) is no longer supported — the S3 target is now BOARD_S3_LCD35B"
#else
#error "No board selected — define BOARD_C6_AMOLED or BOARD_S3_LCD35B in platformio.ini"
#endif

// ---- Derived UI geometry ---------------------------------------------------
// The UI is flex-laid-out and resolution-agnostic almost everywhere; the one
// place that needs a real number is the 2-column overlay menu grid, whose tiles
// were sized for a 368 px panel and would overflow a 320 px one. Derive the
// tile width from the panel instead: full width, less the overlay's 16 px side
// padding, less the 8 px inter-column gap, halved.
#define UI_MENU_TILE_W ((LCD_WIDTH - 16 - 8) / 2)
#define UI_MENU_TILE_H 84

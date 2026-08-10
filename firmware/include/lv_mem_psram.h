// LVGL heap allocator that prefers PSRAM. Used only on boards that have PSRAM —
// see lv_conf.h, which selects this or plain malloc per board.
//
// WHY THIS EXISTS. LVGL allocates every object, style and label buffer from the
// C heap, and they are all small. arduino-esp32's prebuilt config sets
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096, meaning any allocation under 4 KB is
// served from INTERNAL SRAM even when PSRAM is present and empty. This UI has
// hundreds of objects, so the whole widget tree ended up in internal RAM while
// 8 MB of PSRAM sat unused, leaving ~57 KB of internal heap free.
//
// That is not merely wasteful, it crashes: NimBLE creates an esp_timer while
// establishing a connection, the allocation fails, and NimBLE's own
// ESP_ERROR_CHECK calls abort() rather than returning an error — so connecting
// to the load panicked and rebooted the board (ESP_ERR_NO_MEM at
// npl_freertos_callout_init, observed on the 3.5B 2026-08-10).
//
// Routing LVGL to PSRAM puts the widget tree where there is room for it and
// leaves internal SRAM for the things that genuinely need it: the BLE stack,
// DMA-capable buffers, and ISR-context allocations, none of which can live in
// PSRAM.
#pragma once

#include <esp_heap_caps.h>
#include <stdlib.h>

// Fall back to the ordinary heap if PSRAM is exhausted or absent, so a board
// whose PSRAM did not come up still runs rather than failing every allocation.
static inline void *lvPsramAlloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(size);
}

static inline void lvPsramFree(void *p) {
  free(p);   // free() handles any heap the allocator returned
}

static inline void *lvPsramRealloc(void *p, size_t size) {
  void *n = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM);
  return n ? n : realloc(p, size);
}

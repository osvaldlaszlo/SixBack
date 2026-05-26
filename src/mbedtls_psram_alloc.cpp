// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mbedtls_psram_alloc.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace sixback {

// Allocations groesser/gleich diesem Wert gehen nach PSRAM. Schwelle so
// gewaehlt dass die grossen TLS-Buffer (16KB SSL_IN/OUT_CONTENT_LEN)
// + ssl_transform + ssl_handshake_params (jeweils ~1-2KB) sicher in PSRAM
// landen, kleine ephemere Crypto-State-Allocations aber in internal RAM
// bleiben (sicher fuer ggf. DMA-aktive HW-Accelerator-Pfade).
static constexpr size_t kPsramThreshold = 512;

static bool g_installed = false;

static void* hybridCalloc(size_t n, size_t size) {
    const size_t total = n * size;
    if (total >= kPsramThreshold) {
        void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
        // PSRAM exhausted (extrem unwahrscheinlich bei 8MB) → fall through
    }
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void hybridFree(void* p) {
    heap_caps_free(p);
}

void installPsramMbedtlsAllocator() {
    if (g_installed) return;
    if (ESP.getPsramSize() == 0) {
        Serial.println("[mbedtls] PSRAM not available — keeping default internal allocator");
        return;
    }
    int rc = mbedtls_platform_set_calloc_free(hybridCalloc, hybridFree);
    if (rc != 0) {
        Serial.printf("[mbedtls] set_calloc_free failed rc=%d — keeping default\n", rc);
        return;
    }
    g_installed = true;
    Serial.printf("[mbedtls] PSRAM allocator installed (threshold=%u, psram_free=%u KB)\n",
                  (unsigned)kPsramThreshold,
                  (unsigned)(ESP.getFreePsram() / 1024));
}

}  // namespace sixback

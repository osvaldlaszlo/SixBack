// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — mbedtls Allocator-Hook auf PSRAM
//
// Warum: arduino-esp32 baut mbedtls mit CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1
// → mbedtls_ssl_setup() braucht ~32KB kontiguen internal-RAM-Block. Auf einem
// Stick mit min_free=31KB schlaegt das mit -32512 (SSL_MEMORY_ALLOC_FAILED)
// fehl, obwohl 8MB PSRAM ungenutzt sind.
//
// MBEDTLS_PLATFORM_MEMORY ist in esp_config.h definiert → der Runtime-Hook
// mbedtls_platform_set_calloc_free() ist verfuegbar und uebersteuert die
// compile-time-fest verdrahtete esp_mbedtls_mem_calloc.

#pragma once

namespace sixback {

// Installiert einen Hybrid-Allocator fuer mbedtls:
//  - Allocations >= kPsramThreshold gehen nach PSRAM (8MB-Pool)
//  - Kleinere bleiben in internal RAM (sicher fuer AES-HW-DMA u.a.)
//
// Idempotent. No-op wenn kein PSRAM verfuegbar (dann Defaultverhalten).
// Muss VOR jedem TLS-Use aufgerufen werden, idealerweise als erste Zeile
// in setup() nach Serial.begin().
void installPsramMbedtlsAllocator();

}  // namespace sixback

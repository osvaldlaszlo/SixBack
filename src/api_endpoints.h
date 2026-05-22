// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Verwaltungs-API (REST + Mini-HTML) auf Port 80
//
// Diese API ist NICHT für die Speaker — die sprechen das Cloud-Mock
// auf Port 8000 (siehe bose_endpoints.h). Diese hier ist für:
//   - autonomes Testing (curl / WebUI später)
//   - mDNS-discoverable Status
//   - Migrations-Wizard-Trigger
//   - Preset-Verwaltung
#ifndef BOSEFIX32_API_ENDPOINTS_H
#define BOSEFIX32_API_ENDPOINTS_H

#include <ESPAsyncWebServer.h>

void registerApiEndpoints(AsyncWebServer& ui);

#endif // BOSEFIX32_API_ENDPOINTS_H

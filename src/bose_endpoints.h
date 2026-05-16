// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Bose Cloud Replacement Endpoints
//
// Diese Endpoints emuliert der ESP für die Speaker. Verifizierte Liste
// aus den AfterTouch-Live-Logs am Pi5 (siehe /Public/CLAUDE/BOSE/docs/RESEARCH.md §12).
#ifndef BOSEFIX32_BOSE_ENDPOINTS_H
#define BOSEFIX32_BOSE_ENDPOINTS_H

#include <ESPAsyncWebServer.h>

void registerBoseEndpoints(AsyncWebServer& server);

#endif // BOSEFIX32_BOSE_ENDPOINTS_H

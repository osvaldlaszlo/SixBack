// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — global constants
#ifndef BOSEFIX32_CONFIG_H
#define BOSEFIX32_CONFIG_H

// HTTP-Server für Speaker-Anfragen (das was Bose-Cloud früher war)
#define BOSE_HTTP_PORT 8000

// Web-UI / REST-API für User
#define UI_HTTP_PORT 80

// mDNS-Hostname (primary). main.cpp annonciert zusaetzlich legacy "bosefix"
// fuer 30-Tage-Grace nach Rename — siehe MDNS_LEGACY_HOSTNAME.
#define MDNS_HOSTNAME        "sixback"
#define MDNS_LEGACY_HOSTNAME "bosefix"

// Telnet-Port am Speaker (Bose Diagnostic Shell)
#define BOSE_TELNET_PORT 17000

// Bose BMX-API am Speaker
#define BOSE_BMX_PORT 8090

// WiFi-Credentials werden im NVS persistiert (Namespace "sixback-wifi").
// Erstes Provisioning via Improv-Serial (tools/improv_client.py oder
// ESP Web Tools im Browser). Siehe wifi_provisioning.{h,cpp}.

#endif // BOSEFIX32_CONFIG_H

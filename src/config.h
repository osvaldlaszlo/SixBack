// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — global constants
#ifndef BOSEFIX32_CONFIG_H
#define BOSEFIX32_CONFIG_H

// HTTP-Server für Speaker-Anfragen (das was Bose-Cloud früher war)
#define BOSE_HTTP_PORT 8000

// Web-UI / REST-API für User
#define UI_HTTP_PORT 80

// mDNS-Hostname
#define MDNS_HOSTNAME "bosefix"

// Telnet-Port am Speaker (Bose Diagnostic Shell)
#define BOSE_TELNET_PORT 17000

// Bose BMX-API am Speaker
#define BOSE_BMX_PORT 8090

// WiFi-Credentials werden im NVS persistiert (Namespace "bosefix-wifi").
// Erstes Provisioning via Improv-Serial (tools/improv_client.py oder
// ESP Web Tools im Browser). Siehe wifi_provisioning.{h,cpp}.

#endif // BOSEFIX32_CONFIG_H

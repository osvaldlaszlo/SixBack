// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvs_helper.h"
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <memory>
#include <vector>

extern "C" {
#include "heatshrink/heatshrink_encoder.h"
#include "heatshrink/heatshrink_decoder.h"
}

namespace sixback {

namespace {

// ---------------------------------------------------------------------------
// Transparente Blob-Kompression (heatshrink, vendored src/heatshrink, ISC).
//
// Warum: die JSON-Stores (Presets, Inventory, Libraries) wachsen linear mit
// der Speaker-Zahl; unkomprimiert traegt die 24-KB-NVS-Partition ~7 voll
// belegte Speaker (Blob-Update haelt alt+neu gleichzeitig). On-device
// vermessen 2026-06-07: ROM-tdefl braucht 167.744 B Encoder-State — passt
// nicht in den Heap unter SixBack-Last auf C3/C6. heatshrink w=8/l=4:
// 1.596 B Encoder-State, 548 B Decoder-State, Faktor ~2-3,6 auf Store-JSON,
// 13 ms fuer 12 KB auf dem C6. Einheitlich auf allen 4 Chips.
//
// Blob-Format (Generationen-Kette, in-place-Migration beim ersten Save):
//   STRING-Typ            -> Legacy <= v0.8.14 (nvsLoadJson-Fallback)
//   Blob, beginnt mit '{' -> Klartext-JSON (v0.8.15/16 + kleine Werte)
//   Blob, beginnt mit "HS"-> [0]='H' [1]='S' [2]=(window<<4)|lookahead
//                            [3..6]=Klartextlaenge LE32, [7..]=heatshrink
// JSON beginnt immer mit '{' -> kollisionsfrei zum Magic.
// ---------------------------------------------------------------------------
constexpr uint8_t kHsWindow    = 8;   // 2^8-Fenster: 1,6 KB Encoder-State
constexpr uint8_t kHsLookahead = 4;
constexpr size_t  kHsHeader    = 7;
constexpr size_t  kCompressMin = 512;     // kleine Werte (TuneIn-Cache, Auth,
                                          // Diag) bleiben Klartext — Overhead
                                          // lohnt erst ab Store-Groesse
constexpr size_t  kPlainMax    = 65536;   // Sanity-Limit beim Laden (Partition
                                          // ist 24 KB; mehr = korrupt)

// Komprimiert 'in' in einen geframten Blob (out/outLen). false wenn nicht
// lohnend (Ergebnis nicht kleiner) oder OOM — Caller schreibt dann Klartext.
// Review-Härtung 2026-06-07: alle Buffer via new(std::nothrow) — das
// Framework baut mit -fexceptions, ein ungefangenes bad_alloc aus
// std::vector waere abort() statt des dokumentierten Klartext-Fallbacks.
// Beide Schleifen tragen einen Totalfortschritts-Guard (jede Iteration muss
// inPos ODER outPos bewegen), sonst sauberes false statt Haenger.
bool hsCompress_(const String& in, std::unique_ptr<uint8_t[]>& out, size_t& outLen) {
    const size_t inLen = in.length();
    if (inLen < kHsHeader + 1 || inLen > kPlainMax) return false;  // symmetrisch zum Load-Limit
    out.reset(new (std::nothrow) uint8_t[inLen]);
    if (!out) return false;
    heatshrink_encoder* hse = heatshrink_encoder_alloc(kHsWindow, kHsLookahead);
    if (!hse) { out.reset(); return false; }
    out[0] = 'H'; out[1] = 'S';
    out[2] = (uint8_t)((kHsWindow << 4) | kHsLookahead);
    out[3] = (uint8_t)(inLen & 0xFF);
    out[4] = (uint8_t)((inLen >> 8) & 0xFF);
    out[5] = (uint8_t)((inLen >> 16) & 0xFF);
    out[6] = (uint8_t)((inLen >> 24) & 0xFF);
    size_t inPos = 0, outPos = kHsHeader;
    bool ok = true;
    while (inPos < inLen && ok) {
        const size_t inPosBefore = inPos, outPosBefore = outPos;
        size_t n = 0;
        heatshrink_encoder_sink(hse, (uint8_t*)in.c_str() + inPos, inLen - inPos, &n);
        inPos += n;
        HSE_poll_res pr;
        do {
            if (outPos >= inLen) { ok = false; break; }   // wird nicht kleiner
            size_t o = 0;
            pr = heatshrink_encoder_poll(hse, out.get() + outPos, inLen - outPos, &o);
            outPos += o;
        } while (pr == HSER_POLL_MORE);
        if (ok && inPos == inPosBefore && outPos == outPosBefore) {
            ok = false;   // kein Fortschritt -> nie busy-spinnen
        }
    }
    while (ok) {
        HSE_finish_res fr = heatshrink_encoder_finish(hse);
        if (fr == HSER_FINISH_DONE) break;
        if (fr != HSER_FINISH_MORE) { ok = false; break; }
        if (outPos >= inLen)        { ok = false; break; }
        size_t o = 0;
        heatshrink_encoder_poll(hse, out.get() + outPos, inLen - outPos, &o);
        if (o == 0) { ok = false; break; }   // FINISH_MORE ohne Output = stuck
        outPos += o;
    }
    heatshrink_encoder_free(hse);
    if (!ok || outPos >= inLen) { out.reset(); return false; }
    outLen = outPos;
    return true;
}

// Entpackt einen "HS"-geframten Blob nach 'out' (NUL-terminiert, Laenge =
// plainLen aus dem Frame). Window/Lookahead kommen aus dem Frame —
// vorwaertskompatibel falls die Parameter je geaendert werden. false bei
// jedem Decode-/Konsistenz-/OOM-Fehler; haengt NIE (Totalfortschritts-Guard
// — Review-Befund 2026-06-07: ein inkonsistenter Frame mit zu kleinem
// plainLen liess die sink-Schleife sonst endlos spinnen -> WDT-Boot-Loop).
bool hsDecompress_(const uint8_t* in, size_t inLen, std::unique_ptr<char[]>& out) {
    if (inLen < kHsHeader || in[0] != 'H' || in[1] != 'S') return false;
    const uint8_t window    = in[2] >> 4;
    const uint8_t lookahead = in[2] & 0x0F;
    const size_t plainLen = (size_t)in[3] | ((size_t)in[4] << 8)
                          | ((size_t)in[5] << 16) | ((size_t)in[6] << 24);
    if (plainLen == 0 || plainLen > kPlainMax) {
        Serial.printf("[nvs-hs] reject: plainLen=%u\n", (unsigned)plainLen);
        return false;
    }
    if (window < 4 || window > 15 || lookahead < 2 || lookahead >= window) {
        Serial.printf("[nvs-hs] reject: params w=%u l=%u\n", window, lookahead);
        return false;
    }
    out.reset(new (std::nothrow) char[plainLen + 1]);
    if (!out) return false;
    heatshrink_decoder* hsd = heatshrink_decoder_alloc(256, window, lookahead);
    if (!hsd) { out.reset(); return false; }
    size_t cPos = kHsHeader, dPos = 0;
    bool ok = true;
    while (cPos < inLen && ok) {
        const size_t cPosBefore = cPos, dPosBefore = dPos;
        size_t n = 0;
        heatshrink_decoder_sink(hsd, (uint8_t*)in + cPos, inLen - cPos, &n);
        cPos += n;
        HSD_poll_res pr = HSDR_POLL_EMPTY;
        do {
            if (dPos >= plainLen) break;   // erwartete Laenge erreicht
            size_t o = 0;
            pr = heatshrink_decoder_poll(hsd, (uint8_t*)out.get() + dPos, plainLen - dPos, &o);
            dPos += o;
        } while (pr == HSDR_POLL_MORE);
        if (pr == HSDR_POLL_ERROR_NULL || pr == HSDR_POLL_ERROR_UNKNOWN) ok = false;
        if (ok && cPos == cPosBefore && dPos == dPosBefore) {
            // Kein Fortschritt: Input-Buffer voll (sink n=0) und Output am
            // (deklarierten) Ende, aber Komprimat uebrig -> Frame luegt ueber
            // plainLen. Sauber ablehnen statt WDT-Boot-Loop.
            Serial.println("[nvs-hs] reject: surplus input, frame inconsistent");
            ok = false;
        }
    }
    while (ok && dPos < plainLen) {
        HSD_finish_res fr = heatshrink_decoder_finish(hsd);
        if (fr != HSDR_FINISH_MORE) break;
        size_t o = 0;
        heatshrink_decoder_poll(hsd, (uint8_t*)out.get() + dPos, plainLen - dPos, &o);
        dPos += o;
        if (o == 0) break;
    }
    heatshrink_decoder_free(hsd);
    if (!ok || dPos != plainLen) {
        Serial.printf("[nvs-hs] decode FAIL: got %u, want %u\n",
                      (unsigned)dPos, (unsigned)plainLen);
        out.reset();
        return false;
    }
    out[plainLen] = '\0';
    return true;
}

} // namespace

bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc) {
    Preferences p;
    if (!p.begin(ns, true)) return false;
    // Seit dem Blob-Umbau (2026-06-07) liegen JSON-Payloads als NVS-BLOB —
    // nvs_set_blob ist ueber Pages gechunkt, das harte 4000-B-Limit von
    // nvs_set_str entfaellt. Bestands-Daten (<= v0.8.14) liegen noch als
    // STRING unter demselben Key -> Fallback; der naechste Save ersetzt den
    // Eintrag typ-wechselnd durch ein Blob (NVS: neuer Wert ersetzt Typ+Wert).
    size_t blen = p.getBytesLength(key);
    if (blen > 0) {
        std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[blen + 1]);
        if (!buf) { p.end(); return false; }
        size_t got = p.getBytes(key, buf.get(), blen);
        p.end();
        if (got != blen) return false;
        buf[blen] = '\0';
        // "HS"-Frame -> heatshrink-komprimiert (seit Kompressions-Stufe);
        // sonst Klartext-JSON-Blob (v0.8.15/16 + kleine Werte).
        if (blen >= kHsHeader && buf[0] == 'H' && buf[1] == 'S') {
            std::unique_ptr<char[]> plain;
            if (!hsDecompress_(buf.get(), blen, plain)) {
                Serial.printf("[nvs-load] FAIL hs-decode ns=%s key=%s blob=%u\n",
                              ns, key, (unsigned)blen);
                return false;
            }
            // const char* erzwingt Copy-Mode in ArduinoJson (kein zero-copy
            // in den gleich freigegebenen Buffer).
            return deserializeJson(doc, (const char*)plain.get())
                   == DeserializationError::Ok;
        }
        return deserializeJson(doc, (const char*)buf.get())
               == DeserializationError::Ok;
    }
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) return false;
    return deserializeJson(doc, s) == DeserializationError::Ok;
}

bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc) {
    String s;
    serializeJson(doc, s);
    if (s.length() == 0) return false;
    Preferences p;
    if (!p.begin(ns, false)) {
        Serial.printf("[nvs-save] FAIL begin ns=%s key=%s\n", ns, key);
        return false;
    }
    // BLOB statt String (Lab-Befund 2026-06-07): nvs_set_str kann max
    // 4000 B inkl. NUL und braucht die Entries zusammenhaengend in EINER
    // Page — der Preset-Store ueberschritt das ab ~5 Speakern und JEDER
    // Save schlug fehl, obwohl die Partition zu 2/3 leer war. nvs_set_blob
    // chunkt ueber Pages; Limit jetzt ~Partitionsgroesse.
    //
    // Groessere Werte zusaetzlich heatshrink-komprimiert (Faktor ~2-3,6 auf
    // Store-JSON) — hebt die Speaker-Decke der 24-KB-Partition entsprechend.
    // Bei OOM / nicht-lohnender Kompression faellt der Save auf Klartext
    // zurueck; der Load erkennt beides am Frame.
    const uint8_t* data = (const uint8_t*)s.c_str();
    size_t         dlen = s.length();
    std::unique_ptr<uint8_t[]> packed;
    size_t packedLen = 0;
    bool hs = false;
    if (dlen >= kCompressMin && hsCompress_(s, packed, packedLen)) {
        data = packed.get();
        dlen = packedLen;
        hs   = true;
    }
    size_t n = p.putBytes(key, data, dlen);
    p.end();
    if (n != dlen) {
        Serial.printf("[nvs-save] FAIL putBytes ns=%s key=%s json_len=%u blob=%u wrote=%u%s\n",
                      ns, key, (unsigned)s.length(), (unsigned)dlen, (unsigned)n,
                      hs ? " (hs)" : "");
        return false;
    }
    Serial.printf("[nvs-save] ok ns=%s key=%s json_len=%u blob=%u%s\n",
                  ns, key, (unsigned)s.length(), (unsigned)dlen,
                  hs ? " (hs)" : " (plain)");
    return true;
}

bool nvsErase(const char* ns, const char* key) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    bool ok = p.remove(key);
    p.end();
    return ok;
}

bool nvsEraseAllInNamespace(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_erase_all(h);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

void nvsGetStatsJson(JsonDocument& out) {
    nvs_stats_t st{};
    esp_err_t e = nvs_get_stats(NULL, &st);
    if (e != ESP_OK) {
        out["error"] = "nvs_get_stats failed";
        return;
    }
    out["used_entries"]      = (uint32_t)st.used_entries;
    out["free_entries"]      = (uint32_t)st.free_entries;
    out["total_entries"]     = (uint32_t)st.total_entries;
    out["namespace_count"]   = (uint32_t)st.namespace_count;
    out["percent_used"]      = (st.total_entries > 0)
                                ? (100.0f * st.used_entries / st.total_entries)
                                : 0.0f;
}

bool nvsSaveJsonWithCleanup(const char* ns, const char* key, JsonDocument& doc) {
    if (nvsSaveJson(ns, key, doc)) return true;
    Serial.printf("[nvs-cleanup] %s/%s save fail — pass1 purging caches + retry\n", ns, key);
    // Pass 1: Cache-Namespaces wegpurgen (regenerable, kein User-Verlust).
    nvsEraseAllInNamespace("sixback-tune");      // TuneIn-Resolver-Cache
    nvsEraseAllInNamespace("sixback-sys");       // Health/Counters
    if (nvsSaveJson(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass1 retry %s/%s -> OK\n", ns, key);
        return true;
    }
    // Pass 2: aggressivere Liste — Snapshot-Persistenz, Keepalive-State.
    // Beides regenerable beim naechsten Pre-Migrate / Boot. Loescht nicht
    // sixback-{pre,inv,spot,wifi,auto,net} — die haben User-Daten.
    Serial.printf("[nvs-cleanup] %s/%s pass1-fail — pass2 wider purge\n", ns, key);
    nvsEraseAllInNamespace("sixback-snapshot");  // Diag-Snapshots im Flash
    nvsEraseAllInNamespace("sixback-keepalive"); // Marge-Keepalive-State
    nvsEraseAllInNamespace("sixback-c");         // Captive-Stub-State
    nvsEraseAllInNamespace("sixback-s");         // Speaker-Discovery-Cache
    nvsEraseAllInNamespace("sixback-esp");       // ESP-Web-Tools-Cache (falls vorhanden)
    if (nvsSaveJson(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass2 retry %s/%s -> OK (aggressive purge)\n", ns, key);
        return true;
    }
    // Pass 3: den ZIEL-Key explizit loeschen + retry. Hilft wenn NVS-internes
    // GC die alte Version noch nicht reclaim't hat.
    //
    // ABER (Lab-Befund 2026-06-07): NIEMALS den letzten guten Stand opfern.
    // Die alte Fassung loeschte den Key bedingungslos — wenn der Neuschreib
    // dann ebenfalls scheiterte (damals: String > 4000 B = IMMER), war der
    // letzte persistierte Stand vernichtet -> Total-Preset-Verlust nach dem
    // naechsten Reboot. Deshalb: alten Wert vorher sichern und bei erneutem
    // Fehlschlag zurueckschreiben (die soeben freigegebenen Entries reichen
    // dafuer sicher aus).
    Serial.printf("[nvs-cleanup] %s/%s pass2-fail — pass3 erase target-key + retry\n", ns, key);
    // Backup-Buffer nothrow (Review 2026-06-07): ein bad_alloc-Abort genau
    // hier wuerde den Alt-Stand zwischen remove und rewrite vernichten.
    // Wenn das Backup nicht allozierbar ist, wird Pass 3 uebersprungen —
    // lieber Save-Fail als Datenverlust.
    std::unique_ptr<uint8_t[]> oldBlob;
    size_t oldBlobLen = 0;
    String oldStr;
    bool backupOk = true;
    {
        Preferences p;
        if (p.begin(ns, true)) {
            size_t blen = p.getBytesLength(key);
            if (blen > 0) {
                oldBlob.reset(new (std::nothrow) uint8_t[blen]);
                if (oldBlob && p.getBytes(key, oldBlob.get(), blen) == blen) {
                    oldBlobLen = blen;
                } else {
                    oldBlob.reset();
                    backupOk = false;
                }
            } else {
                oldStr = p.getString(key, "");
            }
            p.end();
        }
    }
    if (!backupOk) {
        Serial.printf("[nvs-cleanup] pass3 %s/%s SKIPPED (backup alloc fail) — alter Stand bleibt\n",
                      ns, key);
        return false;
    }
    {
        Preferences p;
        if (p.begin(ns, false)) { p.remove(key); p.end(); }
    }
    bool ok = nvsSaveJson(ns, key, doc);
    if (!ok && (oldBlobLen > 0 || oldStr.length() > 0)) {
        Preferences p;
        bool restored = false;
        if (p.begin(ns, false)) {
            if (oldBlobLen > 0) {
                restored = p.putBytes(key, oldBlob.get(), oldBlobLen) == oldBlobLen;
            } else {
                restored = p.putString(key, oldStr) > 0;
            }
            p.end();
        }
        Serial.printf("[nvs-cleanup] pass3 %s/%s restore alter Stand: %s\n",
                      ns, key, restored ? "OK" : "FAIL — Daten verloren!");
    }
    Serial.printf("[nvs-cleanup] pass3 retry %s/%s -> %s%s\n", ns, key,
                  ok ? "OK" : "STILL-FAIL",
                  ok ? "" : " — NVS partition genuinely full");
    return ok;
}

namespace {

// Liefert true wenn der Namespace mindestens einen Eintrag hat.
bool nvsNamespaceHasEntries(const char* ns) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == nullptr) {
        if (it) nvs_release_iterator(it);
        return false;
    }
    nvs_release_iterator(it);
    return true;
}

// Kopiert einen einzelnen Eintrag basierend auf seinem Typ. Gibt true bei
// Erfolg. Auf Fehler wird der Eintrag uebersprungen und false zurueckgegeben.
bool copyEntry(nvs_handle_t src, nvs_handle_t dst, const nvs_entry_info_t& info) {
    switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t  v; if (nvs_get_u8 (src, info.key, &v) == ESP_OK) return nvs_set_u8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I8:  { int8_t   v; if (nvs_get_i8 (src, info.key, &v) == ESP_OK) return nvs_set_i8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(src, info.key, &v) == ESP_OK) return nvs_set_u16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I16: { int16_t  v; if (nvs_get_i16(src, info.key, &v) == ESP_OK) return nvs_set_i16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(src, info.key, &v) == ESP_OK) return nvs_set_u32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I32: { int32_t  v; if (nvs_get_i32(src, info.key, &v) == ESP_OK) return nvs_set_i32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(src, info.key, &v) == ESP_OK) return nvs_set_u64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I64: { int64_t  v; if (nvs_get_i64(src, info.key, &v) == ESP_OK) return nvs_set_i64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            char* tmp = (char*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_str(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_str(dst, info.key, tmp) == ESP_OK;
            free(tmp);
            return ok;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            if (nvs_get_blob(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            uint8_t* tmp = (uint8_t*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_blob(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_blob(dst, info.key, tmp, len) == ESP_OK;
            free(tmp);
            return ok;
        }
        default:
            return false;
    }
    return false;
}

} // namespace

bool migrateNvsNamespace(const char* oldNs, const char* newNs) {
    // No-op wenn alte Daten gar nicht da sind.
    if (!nvsNamespaceHasEntries(oldNs)) return true;

    // Idempotent: wenn neue Namespace schon Daten hat, gilt Migration als
    // abgeschlossen. Wir loeschen die alte aber NICHT — Sicherheits-Backup
    // bleibt einen Boot lang erhalten falls beim Schreiben in neue NS was
    // schiefging und der naechste Boot retry will. Erst beim 2. Boot mit
    // sauberer neuer NS wird die alte tot.
    if (nvsNamespaceHasEntries(newNs)) {
        // Beide vorhanden: alte ist Cruft, wegputzen.
        nvs_handle_t h;
        if (nvs_open(oldNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            Serial.printf("[nvs-migrate] %s already migrated, erased leftover\n", oldNs);
        }
        return true;
    }

    nvs_handle_t src = 0, dst = 0;
    if (nvs_open(oldNs, NVS_READWRITE, &src) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", oldNs);
        return false;
    }
    if (nvs_open(newNs, NVS_READWRITE, &dst) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", newNs);
        nvs_close(src);
        return false;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", oldNs, NVS_TYPE_ANY, &it);
    int copied = 0, failed = 0;
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (copyEntry(src, dst, info)) {
            copied++;
        } else {
            failed++;
            Serial.printf("[nvs-migrate] copy fail %s/%s (type=%d)\n",
                          oldNs, info.key, (int)info.type);
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    if (failed == 0 && copied > 0) {
        nvs_commit(dst);
        nvs_erase_all(src);
        nvs_commit(src);
        Serial.printf("[nvs-migrate] %s -> %s (%d keys)\n", oldNs, newNs, copied);
    } else if (copied > 0) {
        nvs_commit(dst);
        Serial.printf("[nvs-migrate] %s -> %s partial (%d ok, %d fail, OLD KEPT)\n",
                      oldNs, newNs, copied, failed);
    }
    nvs_close(src);
    nvs_close(dst);
    return failed == 0;
}

void migrateAllBosefixNvs() {
    static const struct { const char* oldNs; const char* newNs; } map[] = {
        { "bosefix-wifi", "sixback-wifi" },  // ZUERST: WiFi-Creds entscheiden Live-Status
        { "bosefix-pre",  "sixback-pre"  },  // User-Presets
        { "bosefix-inv",  "sixback-inv"  },  // Speaker-Inventory
        { "bosefix-auto", "sixback-auto" },  // Auto-Mode-Settings
        { "bosefix-net",  "sixback-net"  },  // IP-Failsafe
        { "bosefix-sys",  "sixback-sys"  },  // Health-Counters
        { "bosefix-tune", "sixback-tune" },  // TuneIn-Cache (unkritisch)
    };
    for (auto& m : map) {
        migrateNvsNamespace(m.oldNs, m.newNs);
    }
}

} // namespace sixback

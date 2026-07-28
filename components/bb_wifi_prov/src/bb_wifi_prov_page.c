// bb_wifi_prov_page — pure, host-testable renderer for the no-callback
// /save success page (B1-809). See bb_wifi_prov.h for the full contract;
// this file owns only string building, no platform/HTTP calls, so it is
// exercised the same way by the device handler (bb_wifi_prov.c's
// prov_save_handler) and by host tests -- one code path, no mirror.

#include "bb_wifi_prov.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

size_t bb_wifi_prov_html_escape(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;

    size_t w = 0;
    if (src) {
        for (const char *p = src; *p != '\0'; p++) {
            const char *ent = NULL;
            size_t ent_len = 0;
            switch (*p) {
            case '&':  ent = "&amp;";  ent_len = 5; break;
            case '<':  ent = "&lt;";   ent_len = 4; break;
            case '>':  ent = "&gt;";   ent_len = 4; break;
            case '"':  ent = "&quot;"; ent_len = 6; break;
            case '\'': ent = "&#39;";  ent_len = 5; break;
            default: break;
            }
            if (ent) {
                // Never emit a partial entity -- stop cleanly instead of
                // truncating mid-escape (would corrupt the markup).
                if (w + ent_len >= out_size) break;
                memcpy(out + w, ent, ent_len);
                w += ent_len;
            } else {
                if (w + 1 >= out_size) break;
                out[w++] = *p;
            }
        }
    }
    out[w] = '\0';
    return w;
}

// snprintf() returns the length the string WOULD have needed, not what was
// actually written -- clamp to what actually landed in out_size so callers
// (and tests) see the real byte count, matching bb_wifi_prov_html_escape's
// convention above.
static size_t clamp_snprintf_result(int rc, size_t out_size)
{
    if (rc < 0) return 0;
    if ((size_t)rc >= out_size) return out_size - 1;
    return (size_t)rc;
}

size_t bb_wifi_prov_render_saved_page(char *out, size_t out_size,
                                      const char *ssid, const char *hostname)
{
    if (!out || out_size == 0) return 0;

    char ssid_esc[BB_WIFI_PROV_HTML_ESCAPED_MAX];
    char host_esc[BB_WIFI_PROV_HTML_ESCAPED_MAX];
    bb_wifi_prov_html_escape(ssid, ssid_esc, sizeof(ssid_esc));
    bb_wifi_prov_html_escape(hostname, host_esc, sizeof(host_esc));

    bool has_hostname = hostname && hostname[0] != '\0';

    // Built separately so the final snprintf below has a single, uniform
    // %s slot regardless of whether a hostname was set -- avoids threading
    // conditional fragments through one long format string.
    //
    // LOW review fix: sized from BB_WIFI_PROV_HTML_ESCAPED_MAX (host_esc
    // appears TWICE in this block's format string) plus headroom for the
    // block's own fixed markup (~134 bytes measured), not a flat magic
    // constant -- a prior flat 512 could be smaller than the true worst
    // case (2 * 200 + 134 = 534), letting this snprintf's own truncation
    // split an already-escaped entity (e.g. "&quot;") for a pathological
    // all-quote hostname, breaking the documented never-partial-entity
    // invariant even though bb_wifi_prov_html_escape() itself never does.
    char host_block[2 * BB_WIFI_PROV_HTML_ESCAPED_MAX + 160] = "";
    if (has_hostname) {
        snprintf(host_block, sizeof(host_block),
                 "<p>Hostname set to <strong>%s</strong>.</p>"
                 "<p>Once reconnected to your network, find this device at "
                 "<strong>http://%s.local/</strong>.</p>",
                 host_esc, host_esc);
    }

    int rc = snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>Wi-Fi Saved</title>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}</style>"
        "</head><body>"
        "<h2>Wi-Fi credentials saved</h2>"
        "<p>This device is connecting to <strong>%s</strong> and will disconnect "
        "this device's own setup network.</p>"
        "%s"
        "<p><strong>This Wi-Fi network is about to disappear</strong> as the "
        "device switches over -- your browser will lose its connection to this "
        "page. That is expected, not an error.</p>"
        "</body></html>",
        ssid_esc, host_block);

    return clamp_snprintf_result(rc, out_size);
}

// MEDIUM review fix: guards bb_wifi_prov_render_saved_page() against
// reporting a hostname that bb_settings_hostname_set() actually rejected --
// prov_save_handler (platform/espidf/bb_wifi_prov/bb_wifi_prov.c) discards an
// invalid hostname (logs, keeps the prior value) but must never tell the
// confirmation page to say "find this device at X.local" for an X that was
// never persisted. Pure/host-testable; called with the result of the
// device handler's own bb_settings_hostname_set() call -- one code path, no
// mirror.
const char *bb_wifi_prov_saved_page_hostname(const char *hostname, bool hostname_saved)
{
    return hostname_saved ? hostname : "";
}

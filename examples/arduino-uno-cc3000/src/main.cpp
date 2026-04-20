#include <Arduino.h>
#include <Adafruit_CC3000.h>
#include "log_stream.h"
#include "nv_config.h"
#include "secrets.h"

static const char *TAG = "bb-uno";

// Adafruit CC3000 shield pin mapping (both v1.0 and v1.1 shields)
static Adafruit_CC3000 cc3000(10 /*CS*/, 3 /*IRQ*/, 5 /*VBAT*/,
                              SPI_CLOCK_DIVIDER);

void setup() {
    Serial.begin(115200);
    while (!Serial);

    bb_nv_config_init();
    bb_log_i(TAG, "boot");

    // Round-trip a value through bb_nv_* to prove EEPROM backend works.
    uint32_t boot_count = 0;
    bb_nv_get_u32("app", "boot", &boot_count, 0);
    boot_count++;
    bb_nv_set_u32("app", "boot", boot_count);
    bb_log_i(TAG, "boot=%lu", (unsigned long)boot_count);

    bb_log_i(TAG, "cc3000 begin...");
    if (!cc3000.begin()) {
        bb_log_e(TAG, "cc3000 begin failed");
        while (1);
    }
    bb_log_i(TAG, "cc3000 begin ok");

    uint8_t major, minor;
    if (!cc3000.getFirmwareVersion(&major, &minor)) {
        bb_log_e(TAG, "fw version read failed");
    } else {
        bb_log_i(TAG, "fw %u.%u", major, minor);
    }

    if (!cc3000.connectToAP(WIFI_SSID, WIFI_PASS, WLAN_SEC_WPA2)) {
        bb_log_e(TAG, "wifi assoc failed");
        while (1);
    }

    while (!cc3000.checkDHCP()) {
        delay(100);
    }

    uint32_t ip, nm, gw, dhcp, dns;
    if (cc3000.getIPAddress(&ip, &nm, &gw, &dhcp, &dns)) {
        bb_log_i(TAG, "ip %lu.%lu.%lu.%lu",
                 (unsigned long)((ip >> 24) & 0xff),
                 (unsigned long)((ip >> 16) & 0xff),
                 (unsigned long)((ip >> 8) & 0xff),
                 (unsigned long)(ip & 0xff));
    }

    bb_log_i(TAG, "online");
}

void loop() {
    // Create server socket on port 80
    static Adafruit_CC3000_Server server(80);
    static bool server_started = false;

    if (!server_started) {
        server.begin();
        server_started = true;
        bb_log_i(TAG, "listening on :80");
    }

    // Check for incoming client
    Adafruit_CC3000_ClientRef client = server.available();
    if (client) {
        // Read HTTP request line
        char method[16] = {0};
        char path[64] = {0};
        int bytes_read = 0;
        bool got_request = false;

        // Read until we see CRLFCRLF
        String request = "";
        unsigned long timeout = millis() + 5000;  // 5 second timeout
        while (millis() < timeout && client.connected()) {
            if (client.available()) {
                char c = client.read();
                request += c;

                // Check for end of headers (CRLFCRLF)
                if (request.endsWith("\r\n\r\n")) {
                    got_request = true;
                    break;
                }
            }
        }

        if (got_request) {
            // Parse the first line: METHOD PATH HTTP/1.x
            int space1 = request.indexOf(' ');
            int space2 = request.indexOf(' ', space1 + 1);
            int crlf = request.indexOf('\r');

            if (space1 > 0 && space2 > space1 && crlf > space2) {
                request.substring(0, space1).toCharArray(method, sizeof(method));
                request.substring(space1 + 1, space2).toCharArray(path, sizeof(path));

                // Check for GET /ping
                if (strcmp(method, "GET") == 0 && strcmp(path, "/ping") == 0) {
                    // Send HTTP response
                    client.fastrprint("HTTP/1.1 200 OK\r\n");
                    client.fastrprint("Content-Type: text/plain\r\n");
                    client.fastrprint("Content-Length: 5\r\n");
                    client.fastrprint("Connection: close\r\n");
                    client.fastrprint("\r\n");
                    client.fastrprint("pong\n");

                    bb_log_i(TAG, "GET /ping -> pong");
                } else {
                    // 404 for anything else
                    client.fastrprint("HTTP/1.1 404 Not Found\r\n");
                    client.fastrprint("Content-Length: 0\r\n");
                    client.fastrprint("Connection: close\r\n");
                    client.fastrprint("\r\n");

                    bb_log_i(TAG, "GET %s -> 404", path);
                }
            }
        }

        client.close();
    }

    delay(10);
}

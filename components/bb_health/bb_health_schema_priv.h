#pragma once

// Private: shared between platform/espidf and platform/host bb_health implementations.
// Split of the /api/health 200 JSON-Schema into base + suffix so extender fragments
// can be injected between them at init time.

static const char k_health_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"validated\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"mdns\":{\"type\":[\"string\",\"null\"]}}}";

static const char k_health_suffix[] =
    "},"
    "\"required\":[\"ok\",\"network\"]}";

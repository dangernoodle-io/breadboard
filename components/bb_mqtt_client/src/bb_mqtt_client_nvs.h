#pragma once
// Internal NVS addressing for bb_mqtt_client — not public API, not shared.
// Byte-compat carryovers from bb_nv's former BB_MQTT_NVS_NS / BB_NV_KEY_CLIENT_ID;
// do not change the string VALUES without an NVS migration plan (strands
// provisioned-board MQTT config otherwise).
#define BB_MQTT_CLIENT_NVS_NS            "bb_mqtt"
#define BB_MQTT_CLIENT_NVS_KEY_CLIENT_ID "client_id"

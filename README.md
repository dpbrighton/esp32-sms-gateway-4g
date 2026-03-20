# ESP32 SMS Gateway 4G

A fully featured, network-independent SMS gateway running on an ESP32 microcontroller with a SIMCom A7670E LTE Cat-1 module. Designed to replace tethered USB GSM modems with a standalone WiFi-connected service.

## Features

- Send and receive SMS via LTE Cat-1
- Browser-based Web UI
- REST API with Bearer token authentication
- WebSocket real-time push notifications
- Webhook — HTTP POST to any URL on inbound SMS
- Full MQTT integration with Home Assistant auto-discovery (16 entities)
- OTA firmware updates via web UI — no USB required
- WiFi log monitor — stream ESP32 logs over the network
- NVS-backed configuration — survives reboots and firmware updates

## Hardware

| Component | Details |
|-----------|---------|
| ESP32-D0WD-V3 | ESP32 DevKit board, dual core 240MHz |
| SIMCom A7670E | BK-7670 V1 breakout board, LTE Cat-1 |
| SIM Card | Standard nano/micro SIM with SMS capability |

### Wiring

| ESP32 Pin | BK-7670 Pin | Function |
|-----------|-------------|----------|
| GPIO 17 | R (RXD) | ESP32 TX → Modem RX |
| GPIO 16 | T (TXD) | Modem TX → ESP32 RX |
| GPIO 4 | K (PWRKEY) | Power key control |
| GPIO 5 | S (RESET) | Hardware reset |
| GND | G (GND) | Common ground |

Power each board independently via USB — do NOT connect the V pin.

## Quick Start

### Prerequisites

- macOS with Homebrew
- ESP-IDF v6.1

Run the install script to set up everything automatically:
```bash
./install_macos.sh
```

### Configuration

Open `components/config/config.c` and update the defaults:
```c
strlcpy(s_config.wifi_ssid,         "YOUR_WIFI_SSID",       sizeof(...));
strlcpy(s_config.wifi_password,     "YOUR_WIFI_PASSWORD",   sizeof(...));
strlcpy(s_config.mqtt_broker_uri,   "mqtt://YOUR_BROKER_IP:1883", sizeof(...));
strlcpy(s_config.mqtt_topic_prefix, "sms_gateway_4g",       sizeof(...));
strlcpy(s_config.mqtt_username,     "YOUR_MQTT_USERNAME",   sizeof(...));
strlcpy(s_config.mqtt_password,     "YOUR_MQTT_PASSWORD",   sizeof(...));
strlcpy(s_config.api_key,           "YOUR_API_KEY",         sizeof(...));
```

### Build and Flash
```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-XXXXX flash monitor
```

## Home Assistant Integration

The gateway uses MQTT auto-discovery. Once running, a device called **SMS Gateway 4G** appears automatically in HA under Settings → Devices with 16 entities:

**Controls:** Recipient Number, Outgoing Message, Send SMS button

**Sensors:** Gateway Status, Last Sender, Last Message, Last Received Time, Send Status, Last Sent To, Last Sent Message

**Diagnostic:** Signal Strength, Network Operator, Messages Sent, Messages Received

### Sending SMS from an HA Automation

Publish to `sms_gateway_4g/send`:
```json
{"number": "+447700900000", "body": "Front door opened"}
```

### Receiving SMS — Triggering Automations

Add a trigger in the HA automation editor:
**Device → SMS Gateway 4G → Message Received**

Access payload fields with:
- `trigger.payload_json.from`
- `trigger.payload_json.body`
- `trigger.payload_json.timestamp`

## WiFi Log Monitor

Stream ESP32 logs over the network without USB:
```bash
nc <gateway-ip> 8888
```

## REST API

| Method | Endpoint | Description | Auth |
|--------|----------|-------------|------|
| GET | /api/status | Modem status | No |
| GET | /api/sms/inbox | Inbox messages | No |
| GET | /api/sms/outbox | Sent messages | No |
| POST | /api/sms/send | Send SMS {to, body} | Yes |
| GET | /api/config | Read config | Yes |
| POST | /api/config | Update config | Yes |
| POST | /api/ota/update | Start OTA update | Yes |

## Project Structure
```
sms_gateway_4g/
  main/               # App entry point
  components/
    a7670e/           # AT command modem driver
    sms_store/        # In-memory SMS store
    config/           # NVS-backed configuration
    web_server/       # HTTP server + Web UI + WebSocket
    rest_api/         # REST endpoint handlers
    mqtt_gateway/     # MQTT client + HA auto-discovery
    webhook/          # HTTP POST on inbound SMS
    tcp_log/          # WiFi log streaming on port 8888
    ota_update/       # OTA firmware updates
```

## Documentation

Full project documentation including hardware setup, all interfaces, debugging guide and development notes is available in the project docs folder.

## License

MIT License — see LICENSE file for details.

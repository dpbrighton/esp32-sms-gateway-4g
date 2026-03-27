#!/usr/bin/env python3
"""
MQTT Heartbeat Test
Connects to the same broker as the ESP32 and monitors connection stability.
If this disconnects, the problem is broker/network side, not ESP32 RF.
"""

import paho.mqtt.client as mqtt
import time
import datetime

BROKER   = "192.168.0.x"  # Replace with your MQTT broker IP
PORT     = 1883
USERNAME = "your-mqtt-username"  # Replace with your MQTT username
PASSWORD = "your-mqtt-password"  # Replace with your MQTT password
KEEPALIVE = 120  # Same as ESP32

connected = False
connect_time = None
disconnect_count = 0

def log(msg):
    print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {msg}")

def on_connect(client, userdata, flags, rc):
    global connected, connect_time
    connected = True
    connect_time = time.time()
    log(f"CONNECTED to broker (rc={rc})")

def on_disconnect(client, userdata, rc):
    global connected, disconnect_count
    connected = False
    disconnect_count += 1
    uptime = time.time() - connect_time if connect_time else 0
    log(f"DISCONNECTED (rc={rc}) after {uptime:.0f}s uptime — total disconnects: {disconnect_count}")

def on_log(client, userdata, level, buf):
    if "PINGREQ" in buf or "PINGRESP" in buf:
        log(f"PING: {buf}")

client = mqtt.Client(client_id="mac_heartbeat_test")
client.username_pw_set(USERNAME, PASSWORD)
client.on_connect    = on_connect
client.on_disconnect = on_disconnect

log(f"Connecting to {BROKER}:{PORT} with keepalive={KEEPALIVE}s...")
client.connect(BROKER, PORT, keepalive=KEEPALIVE)

try:
    last_status = time.time()
    client.loop_start()
    while True:
        time.sleep(10)
        if time.time() - last_status >= 60:
            uptime = time.time() - connect_time if connect_time else 0
            log(f"STATUS: {'connected' if connected else 'DISCONNECTED'} — uptime {uptime:.0f}s — disconnects so far: {disconnect_count}")
            last_status = time.time()
except KeyboardInterrupt:
    log("Stopped by user.")
    client.loop_stop()
    client.disconnect()

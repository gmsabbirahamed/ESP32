import paho.mqtt.client as mqtt
import requests
import json

# --- MQTT Config ---
MQTT_BROKER = "iinms.brri.gov.bd"
MQTT_PORT = 1883
MQTT_TOPIC = "sabbir/gprs"

# --- Supabase Config ---
SUPABASE_URL = "http://udlykedkcxtqhnyabnpg.supabase.co"
SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVkbHlrZWRrY3h0cWhueWFibnBnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTA1NzYwNzQsImV4cCI6MjA2NjE1MjA3NH0.vFsQAUPiUrsIrBu96iUNMfMkF4NRt7PuVsdSzKFWZow"
TABLE_NAME = "sensor_readings_"

# --- Supabase Endpoint & Headers ---
SUPABASE_URL_FULL = f"{SUPABASE_URL}/rest/v1/{TABLE_NAME}"
HEADERS = {
    "Content-Type": "application/json",
    "apikey": SUPABASE_ANON_KEY,
    "Authorization": f"Bearer {SUPABASE_ANON_KEY}",
    "Prefer": "return=minimal"
}


def send_to_supabase(data):
    """Send JSON data to Supabase REST API."""
    try:
        response = requests.post(SUPABASE_URL_FULL, headers=HEADERS, data=json.dumps(data))
        if response.status_code in [200, 201, 204]:
            print(f"[✅] Uploaded: {data}")
        else:
            print(f"[❌] Upload failed ({response.status_code}): {response.text}")
    except Exception as e:
        print(f"[⚠️] Error sending to Supabase: {e}")


def on_connect(client, userdata, flags, rc):
    """When MQTT connects successfully."""
    if rc == 0:
        print(f"[MQTT] Connected to {MQTT_BROKER}")
        client.subscribe(MQTT_TOPIC)
        print(f"[MQTT] Subscribed to topic: {MQTT_TOPIC}")
    else:
        print(f"[MQTT] Connection failed with code {rc}")


def on_message(client, userdata, msg):
    """When MQTT receives a message."""
    try:
        payload = msg.payload.decode()
        print(f"[MQTT] Message received: {payload}")

        # Parse incoming JSON
        data = json.loads(payload)
        send_to_supabase(data)

    except json.JSONDecodeError:
        print(f"[⚠️] Invalid JSON: {msg.payload}")


def main():
    """Main MQTT + Supabase bridge."""
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    print("[MQTT] Connecting...")
    client.connect(MQTT_BROKER, MQTT_PORT, 60)

    # Blocking loop to listen for incoming MQTT messages
    client.loop_forever()


if __name__ == "__main__":
    main()

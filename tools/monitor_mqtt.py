#!/usr/bin/env python3
"""Subscribe to Bituo Dial MQTT topics and print payloads.

Usage (Python 3.13):
  pip install paho-mqtt
  python tools/monitor_mqtt.py [host] [port]
  python tools/monitor_mqtt.py 127.0.0.1 1883 --seconds 15

Default topic: bituo-dial/#
"""
from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.stderr.write("need paho-mqtt: pip install paho-mqtt\n")
    sys.exit(1)


def on_connect(client, userdata, flags, reason_code, properties=None):
    topic = userdata["topic"]
    print(f"[{datetime.now():%H:%M:%S}] connected rc={reason_code}, sub {topic}",
          flush=True)
    client.subscribe(topic)


def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="replace")
    print(f"[{datetime.now():%H:%M:%S}] {msg.topic} {payload}", flush=True)
    userdata["count"] = userdata.get("count", 0) + 1
    limit = userdata.get("max_messages") or 0
    if limit > 0 and userdata["count"] >= limit:
        client.disconnect()


def make_client(args):
    userdata = {
        "topic": args.topic,
        "count": 0,
        "max_messages": args.max_messages,
    }
    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, userdata=userdata)
    else:
        client = mqtt.Client(userdata=userdata)
    client.on_connect = on_connect
    client.on_message = on_message
    if args.user:
        client.username_pw_set(args.user, args.password)
    return client


def main() -> int:
    p = argparse.ArgumentParser(description="Monitor bituo-dial MQTT topics")
    p.add_argument("host", nargs="?", default="127.0.0.1")
    p.add_argument("port", nargs="?", type=int, default=1883)
    p.add_argument("--topic", default="bituo-dial/#")
    p.add_argument("--user", default="")
    p.add_argument("--password", default="")
    p.add_argument("--seconds", type=float, default=0,
                   help="exit after N seconds (0 = run until Ctrl+C)")
    p.add_argument("--max-messages", type=int, default=0,
                   help="exit after N messages (0 = unlimited)")
    args = p.parse_args()

    client = make_client(args)
    try:
        client.connect(args.host, args.port, keepalive=30)
    except Exception as e:
        sys.stderr.write(f"connect failed: {e}\n")
        return 1

    print(f"listening {args.host}:{args.port} topic={args.topic}", flush=True)
    if args.seconds > 0:
        t0 = time.time()
        client.loop_start()
        while time.time() - t0 < args.seconds:
            time.sleep(0.2)
        client.loop_stop()
        client.disconnect()
        print("stopped", flush=True)
        return 0

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("stopped", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

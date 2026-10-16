#!/usr/bin/env python3
"""Probe the Govee OpenAPI 2.0 before porting it to the board.

The API key is read from the GOVEE_API_KEY environment variable, never from
the command line or a file in the repository:

    export GOVEE_API_KEY=...        # in your own terminal
    python3 tools/govee_probe.py list
    python3 tools/govee_probe.py state
    python3 tools/govee_probe.py watch --interval 60 --minutes 30

Standard library only.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

BASE = "https://openapi.api.govee.com/router/api/v1"


def call(method, path, body=None):
    key = os.environ.get("GOVEE_API_KEY")
    if not key:
        sys.exit("Set GOVEE_API_KEY in your terminal first.")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header("Govee-API-Key", key)
    req.add_header("Content-Type", "application/json")
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            status, headers, raw = r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        status, headers, raw = e.code, e.headers, e.read()
    ms = (time.monotonic() - t0) * 1000
    limits = {k: v for k, v in headers.items() if "limit" in k.lower()}
    print(f"  HTTP {status}, {len(raw)} bytes, {ms:.0f} ms, limits: {limits or 'none'}")
    try:
        return status, json.loads(raw)
    except ValueError:
        return status, None


def devices():
    _, js = call("GET", "/user/devices")
    return (js or {}).get("data", [])


def state(dev):
    body = {"requestId": str(uuid.uuid4()),
            "payload": {"sku": dev["sku"], "device": dev["device"]}}
    _, js = call("POST", "/device/state", body)
    out = {}
    for cap in ((js or {}).get("payload") or {}).get("capabilities", []):
        out[cap.get("instance")] = (cap.get("state") or {}).get("value")
    return out


def fmt(dev, s):
    t = s.get("sensorTemperature")
    tc = f"{(t - 32) * 5 / 9:.2f}°C ({t}°F)" if isinstance(t, (int, float)) else t
    return (f"{dev['sku']} {dev['deviceName']!r}: online={s.get('online')} "
            f"temp={tc} hum={s.get('sensorHumidity')}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["list", "state", "watch"])
    ap.add_argument("--interval", type=int, default=60, help="seconds (watch)")
    ap.add_argument("--minutes", type=int, default=30, help="duration (watch)")
    a = ap.parse_args()

    print("[list]")
    devs = devices()
    sensors = [d for d in devs if d.get("type") == "devices.types.thermometer"]
    for d in devs:
        print(f"  {d['sku']} {d['device']} {d['deviceName']!r} {d.get('type')}")
    if a.cmd == "list":
        return

    if a.cmd == "state":
        for d in sensors:
            print("[state]")
            print("  " + fmt(d, state(d)))
        return

    # watch: how often does the cloud value actually change?
    last, changes, t_end = {}, 0, time.time() + a.minutes * 60
    while time.time() < t_end:
        for d in sensors:
            s = state(d)
            cur = (s.get("sensorTemperature"), s.get("sensorHumidity"))
            mark = ""
            if d["device"] in last and last[d["device"]] != cur:
                changes += 1
                mark = "  <-- changed"
            last[d["device"]] = cur
            print(time.strftime("%H:%M:%S ") + fmt(d, s) + mark, flush=True)
        time.sleep(a.interval)
    print(f"{changes} changes in {a.minutes} min")


if __name__ == "__main__":
    main()

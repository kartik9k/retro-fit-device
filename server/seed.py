"""
Dummy data generator for server and Azure testing (Mode 2).

Simulates ESP8266 batch POSTs using the exact same wire format as the firmware.
Safe to run against localhost (Mode 1) or the Azure endpoint (Mode 2).

Usage:
  # Mode 2 — Azure
  python seed.py --url https://<app>.<hash>.uksouth.azurecontainerapps.io/api/data

  # Mode 1 — local
  python seed.py --url http://localhost:5000/api/data

  # Continuous streaming (Ctrl-C to stop)
  python seed.py --url http://localhost:5000/api/data --continuous --interval 30

Options:
  --url           POST endpoint  [default: http://localhost:5000/api/data]
  --device        Device name    [default: retro-fit-seed]
  --sensor        Sensor tag     [default: us]
  --count         Total readings to generate (non-continuous mode)  [default: 60]
  --batch-size    Readings per POST (mirrors READING_QUEUE_DEPTH)   [default: 15]
  --interval      Seconds between POSTs in continuous mode          [default: 30]
  --continuous    Run until Ctrl-C, posting every --interval seconds
  --base-level    Base water level in cm  [default: 120.0]
  --noise         Random noise amplitude in cm  [default: 5.0]
"""

import argparse
import json
import math
import random
import sys
import time
import urllib.request
import urllib.error


def _generate_batch(
    base_level: float,
    noise: float,
    batch_size: int,
    sensor: str = "us",
    sensor_period_ms: int = 2000,
) -> list[dict]:
    """
    Generate one batch of readings matching the firmware wire format.
    t values are boot-relative ms, oldest first, spaced by sensor_period_ms.
    v is in cm (the ultrasonic sensor unit).
    Approximately 5 % of readings return null to simulate sensor timeouts.
    """
    boot_offset_ms = random.randint(60_000, 3_600_000)  # pretend device has been up a while
    readings = []
    for i in range(batch_size):
        t = boot_offset_ms + i * sensor_period_ms
        if random.random() < 0.05:
            v = None
        else:
            # Slow sinusoidal drift + gaussian noise
            drift = math.sin(boot_offset_ms / 300_000) * 10
            v = round(base_level + drift + random.gauss(0, noise), 1)
            v = max(2.0, v)  # clamp to sensor minimum range
        readings.append({"k": sensor, "v": v, "t": t})
    return readings


def _post(url: str, payload: dict) -> dict:
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read())


def run(args: argparse.Namespace) -> None:
    print(f"Target : {args.url}")
    print(f"Device : {args.device}  sensor={args.sensor}  proto=1.0")
    print(f"Level  : {args.base_level} cm ± {args.noise} cm noise")

    if args.continuous:
        print(f"Mode   : continuous — posting every {args.interval} s  (Ctrl-C to stop)\n")
        batch_num = 0
        while True:
            batch_num += 1
            readings = _generate_batch(args.base_level, args.noise, args.batch_size, args.sensor)
            payload = {"proto": "1.0", "device": args.device, "readings": readings}
            try:
                result = _post(args.url, payload)
                print(f"[batch {batch_num:4d}]  inserted={result['inserted']}  "
                      f"ids={result['ids'][0]}..{result['ids'][-1]}")
            except urllib.error.URLError as e:
                print(f"[batch {batch_num:4d}]  POST failed: {e}", file=sys.stderr)
            time.sleep(args.interval)
    else:
        total = args.count
        sent = 0
        batch_num = 0
        print(f"Mode   : {total} readings in batches of {args.batch_size}\n")
        while sent < total:
            batch_num += 1
            size = min(args.batch_size, total - sent)
            readings = _generate_batch(args.base_level, args.noise, size, args.sensor)
            payload = {"proto": "1.0", "device": args.device, "readings": readings}
            try:
                result = _post(args.url, payload)
                sent += result["inserted"]
                print(f"[batch {batch_num:3d}]  inserted={result['inserted']}  "
                      f"total sent={sent}/{total}")
            except urllib.error.URLError as e:
                print(f"[batch {batch_num:3d}]  POST failed: {e}", file=sys.stderr)
                sys.exit(1)
        print(f"\nDone — {sent} readings seeded.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Seed dummy sensor readings")
    parser.add_argument("--url", default="http://localhost:5000/api/data")
    parser.add_argument("--device", default="retro-fit-seed")
    parser.add_argument("--sensor", default="us")
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--batch-size", type=int, default=15)
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--continuous", action="store_true")
    parser.add_argument("--base-level", type=float, default=120.0)
    parser.add_argument("--noise", type=float, default=5.0)
    run(parser.parse_args())


if __name__ == "__main__":
    main()

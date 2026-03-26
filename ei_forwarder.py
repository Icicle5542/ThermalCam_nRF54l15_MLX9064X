#!/usr/bin/env python3
"""
ei_forwarder.py — Capture thermal frames from ThermalCam over BLE and
upload them to Edge Impulse Studio as labelled training images.

Each frame is converted to a 32×24 grayscale PNG and POSTed to the
Edge Impulse ingestion API.

Requirements:
    pip install bleak numpy Pillow requests

Usage:
    python ei_forwarder.py --api-key ei_xxxx --label "person" --count 50
    python ei_forwarder.py --api-key ei_xxxx --label "empty"  --count 50 --category testing

Environment variable alternative for the API key:
    set EI_API_KEY=ei_xxxx
    python ei_forwarder.py --label "person" --count 50
"""

import argparse
import asyncio
import os
import struct
import sys
import threading
import time

import numpy as np
from PIL import Image
import requests
from bleak import BleakScanner, BleakClient

# ── NUS characteristic UUIDs ───────────────────────────────────────────────
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# ── Frame / device constants ───────────────────────────────────────────────
DEVICE_NAME   = "ThermalCam"
ROWS, COLS    = 24, 32
PIXELS        = ROWS * COLS
FRAME_PAYLOAD = 4 + PIXELS
FRAME_TOTAL   = 2 + FRAME_PAYLOAD + 2
SOF           = bytes([0xFF, 0xFE])
EOF_MARKER    = bytes([0xFF, 0xFD])

# ── Edge Impulse ingestion endpoint ───────────────────────────────────────
EI_INGESTION_URL = "https://ingestion.edgeimpulse.com/api/training/files"

# ── Shared state ──────────────────────────────────────────────────────────
_lock       = threading.Lock()
_rx_buf     = bytearray()
_new_frame  = threading.Event()        # signalled when a complete frame arrives
_last_frame = np.zeros((ROWS, COLS), dtype=np.uint8)
_t_min      = 0.0
_t_max      = 0.0


def _parse_frames(new_data: bytearray) -> None:
    global _rx_buf, _last_frame, _t_min, _t_max

    _rx_buf.extend(new_data)

    while True:
        sof_idx = _rx_buf.find(SOF)
        if sof_idx < 0:
            _rx_buf = _rx_buf[-1:] if _rx_buf else bytearray()
            break
        if len(_rx_buf) < sof_idx + FRAME_TOTAL:
            _rx_buf = _rx_buf[sof_idx:]
            break

        eof_pos = sof_idx + 2 + FRAME_PAYLOAD
        if _rx_buf[eof_pos : eof_pos + 2] != EOF_MARKER:
            _rx_buf = _rx_buf[sof_idx + 1:]
            continue

        payload = bytes(_rx_buf[sof_idx + 2 : eof_pos])
        _rx_buf = _rx_buf[eof_pos + 2:]

        t_min_raw, t_max_raw = struct.unpack_from(">hh", payload, 0)

        with _lock:
            _t_min = t_min_raw / 10.0
            _t_max = t_max_raw / 10.0
            _last_frame = (
                np.frombuffer(payload[4:], dtype=np.uint8)
                  .reshape(ROWS, COLS)
                  .copy()
            )
        _new_frame.set()


def _on_notification(_sender, data: bytearray) -> None:
    _parse_frames(data)


def _frame_to_png_bytes(frame: np.ndarray) -> bytes:
    """Convert a 24×32 uint8 numpy array to PNG bytes in memory."""
    img = Image.fromarray(frame, mode="L")
    # Upscale to 96×72 (3×) for Edge Impulse — optional, remove if you
    # want the native 32×24 resolution.
    img = img.resize((COLS * 3, ROWS * 3), Image.NEAREST)
    import io
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _upload_frame(png_data: bytes, api_key: str, label: str,
                  category: str, idx: int) -> bool:
    """Upload a single PNG frame to Edge Impulse ingestion API."""
    filename = f"thermal_{label}_{idx:04d}.png"

    headers = {
        "x-api-key": api_key,
        "x-label": label,
    }
    if category == "testing":
        headers["x-add-date-id"] = "1"
        url = EI_INGESTION_URL.replace("/training/", "/testing/")
    else:
        url = EI_INGESTION_URL

    files = {
        "data": (filename, png_data, "image/png"),
    }

    try:
        resp = requests.post(url, headers=headers, files=files, timeout=30)
        if resp.ok:
            print(f"  [{idx}] Uploaded {filename} -> {resp.json().get('success', resp.text)}")
            return True
        else:
            print(f"  [{idx}] Upload failed ({resp.status_code}): {resp.text}")
            return False
    except requests.RequestException as e:
        print(f"  [{idx}] Upload error: {e}")
        return False


async def _ble_coroutine(stop_event: threading.Event) -> None:
    print(f'Scanning for "{DEVICE_NAME}" …')
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if device is None:
        print(f'ERROR: "{DEVICE_NAME}" not found.')
        stop_event.set()
        return

    print(f"Found {device.name} ({device.address}).  Connecting …")

    async with BleakClient(device) as client:
        await client.start_notify(NUS_TX_CHAR_UUID, _on_notification)
        print("Connected.  Receiving frames …")
        while not stop_event.is_set():
            await asyncio.sleep(0.1)
        await client.stop_notify(NUS_TX_CHAR_UUID)

    print("Disconnected.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Capture thermal frames via BLE and upload to Edge Impulse."
    )
    parser.add_argument(
        "--api-key",
        default=os.environ.get("EI_API_KEY", ""),
        help="Edge Impulse API key (or set EI_API_KEY env var).",
    )
    parser.add_argument(
        "--label", required=True,
        help='Label for the uploaded samples (e.g. "person", "empty").',
    )
    parser.add_argument(
        "--count", type=int, default=20,
        help="Number of frames to capture and upload (default: 20).",
    )
    parser.add_argument(
        "--category", choices=["training", "testing"], default="training",
        help="Edge Impulse dataset split (default: training).",
    )
    parser.add_argument(
        "--interval", type=float, default=0.0,
        help="Minimum seconds between captures (default: 0 = every frame).",
    )
    parser.add_argument(
        "--save-local", action="store_true",
        help="Also save PNGs locally in ./ei_captures/<label>/.",
    )
    args = parser.parse_args()

    if not args.api_key:
        print("ERROR: Provide --api-key or set EI_API_KEY environment variable.")
        sys.exit(1)

    # Local save directory
    if args.save_local:
        local_dir = os.path.join("ei_captures", args.label)
        os.makedirs(local_dir, exist_ok=True)

    stop_event = threading.Event()

    def _ble_thread_fn():
        asyncio.run(_ble_coroutine(stop_event))

    ble_thread = threading.Thread(target=_ble_thread_fn, daemon=True)
    ble_thread.start()

    print(f"Will capture {args.count} frames with label \"{args.label}\" "
          f"({args.category}).\n")

    uploaded = 0
    last_upload = 0.0

    try:
        while uploaded < args.count and not stop_event.is_set():
            # Wait for the next complete frame from BLE
            if not _new_frame.wait(timeout=1.0):
                continue
            _new_frame.clear()

            # Honour minimum interval
            now = time.monotonic()
            if args.interval > 0 and (now - last_upload) < args.interval:
                continue

            with _lock:
                frame = _last_frame.copy()

            png_data = _frame_to_png_bytes(frame)
            idx = uploaded + 1

            if args.save_local:
                path = os.path.join("ei_captures", args.label,
                                    f"thermal_{args.label}_{idx:04d}.png")
                with open(path, "wb") as f:
                    f.write(png_data)

            ok = _upload_frame(png_data, args.api_key, args.label,
                               args.category, idx)
            if ok:
                uploaded += 1
                last_upload = now

    except KeyboardInterrupt:
        print("\nInterrupted.")

    stop_event.set()
    ble_thread.join(timeout=5.0)

    print(f"\nDone — uploaded {uploaded}/{args.count} frames "
          f"(label=\"{args.label}\", {args.category}).")


if __name__ == "__main__":
    main()

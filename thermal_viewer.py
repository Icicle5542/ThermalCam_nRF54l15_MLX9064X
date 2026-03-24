#!/usr/bin/env python3
"""
thermal_viewer.py — Real-time BLE thermal camera viewer for MLX90640.

Connects to an nRF54L15 DK running ThermalCam firmware via the Nordic UART
Service (NUS), receives 32 × 24 greyscale frames, and displays them as a
live colour-mapped image using matplotlib.

Wire protocol (776 bytes per frame, big-endian integers):
  Offset  Size  Content
       0     2  SOF  0xFF 0xFE
       2     2  t_min × 10  (int16, e.g. 227 = 22.7 °C)
       4     2  t_max × 10  (int16)
       6   768  uint8 pixels[768]  0 = coldest, 255 = hottest  (row-major)
     774     2  EOF  0xFF 0xFD

Requirements:
    pip install bleak numpy matplotlib

Usage:
    python thermal_viewer.py
"""

import asyncio
import struct
import sys
import threading
import time

import numpy as np
import matplotlib.pyplot as plt
from bleak import BleakScanner, BleakClient

# ── NUS characteristic UUIDs ───────────────────────────────────────────────
# TX = device → PC (notifications); RX = PC → device (write, unused here)
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# ── Frame / device constants ───────────────────────────────────────────────
DEVICE_NAME   = "ThermalCam"
ROWS, COLS    = 24, 32
PIXELS        = ROWS * COLS             # 768
FRAME_PAYLOAD = 4 + PIXELS              # header (4 B) + pixels (768 B) = 772
FRAME_TOTAL   = 2 + FRAME_PAYLOAD + 2  # SOF + payload + EOF = 776
SOF           = bytes([0xFF, 0xFE])
EOF_MARKER    = bytes([0xFF, 0xFD])

# ── Shared state (updated by the BLE notification callback) ───────────────
_lock       = threading.Lock()   # guards _last_frame / _t_min / _t_max
_rx_buf     = bytearray()
_last_frame = np.zeros((ROWS, COLS), dtype=np.uint8)
_t_min      = 0.0
_t_max      = 0.0


def _parse_frames(new_data: bytearray) -> None:
    """
    Append *new_data* to the receive buffer and extract all complete frames.

    The EOF is checked at the fixed, expected offset rather than being
    searched for, so pixel data that happens to contain the SOF byte
    pattern (0xFF 0xFE) does not silently corrupt framing.
    """
    global _rx_buf, _last_frame, _t_min, _t_max  # noqa: PLW0603

    _rx_buf.extend(new_data)

    while True:
        sof_idx = _rx_buf.find(SOF)

        if sof_idx < 0:
            # No SOF anywhere — keep only the last byte as a partial-SOF guard.
            _rx_buf = _rx_buf[-1:] if _rx_buf else bytearray()
            break

        if len(_rx_buf) < sof_idx + FRAME_TOTAL:
            # SOF found but not enough bytes yet for a complete frame.
            _rx_buf = _rx_buf[sof_idx:]
            break

        # Expected EOF position (fixed offset from SOF).
        eof_pos = sof_idx + 2 + FRAME_PAYLOAD

        if _rx_buf[eof_pos : eof_pos + 2] != EOF_MARKER:
            # EOF mismatch at the expected position → false SOF inside pixel
            # data.  Skip past this byte and retry from the next SOF candidate.
            _rx_buf = _rx_buf[sof_idx + 1:]
            continue

        # Valid frame found — extract payload between SOF and EOF.
        payload = bytes(_rx_buf[sof_idx + 2 : eof_pos])
        _rx_buf = _rx_buf[eof_pos + 2:]   # consume the frame from the buffer

        _t_min_raw, _t_max_raw = struct.unpack_from(">hh", payload, 0)

        with _lock:
            _t_min = _t_min_raw / 10.0
            _t_max = _t_max_raw / 10.0
            _last_frame = (
                np.frombuffer(payload[4:], dtype=np.uint8)
                  .reshape(ROWS, COLS)
                  .copy()
            )

        print(
            f"Frame received — "
            f"min {_t_min:.1f} \u00b0C  "
            f"max {_t_max:.1f} \u00b0C  "
            f"span {_t_max - _t_min:.1f} \u00b0C"
        )


def _on_notification(_sender, data: bytearray) -> None:
    _parse_frames(data)


async def _ble_coroutine(stop_event: threading.Event) -> None:
    """BLE task — runs in a background thread's asyncio event loop."""
    print(f'Scanning for "{DEVICE_NAME}" \u2026')
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if device is None:
        print(
            f'ERROR: "{DEVICE_NAME}" not found.\n'
            "Make sure the DK is powered and advertising."
        )
        stop_event.set()
        return

    print(f"Found {device.name} ({device.address}).  Connecting \u2026")

    async with BleakClient(device) as client:
        await client.start_notify(NUS_TX_CHAR_UUID, _on_notification)
        print("Connected.  Receiving frames.  Close the plot window to exit.")
        # Stay connected until the GUI window is closed.
        while not stop_event.is_set():
            await asyncio.sleep(0.1)
        await client.stop_notify(NUS_TX_CHAR_UUID)

    print("Disconnected.")


if __name__ == "__main__":
    stop_event = threading.Event()

    # ── Launch BLE in a background thread so the asyncio loop never competes
    #    with the matplotlib GUI event loop. ──────────────────────────────────
    def _ble_thread_fn():
        asyncio.run(_ble_coroutine(stop_event))

    ble_thread = threading.Thread(target=_ble_thread_fn, daemon=True)
    ble_thread.start()

    # ── Build the matplotlib figure on the main thread. ──────────────────────
    plt.ion()
    fig, ax = plt.subplots(figsize=(7, 5))
    fig.suptitle("MLX90640 Thermal Camera \u2014 Live View", fontsize=12)

    img_plot = ax.imshow(
        _last_frame,
        cmap="inferno",
        vmin=0, vmax=255,
        interpolation="nearest",
        aspect="equal",
    )
    cbar = fig.colorbar(img_plot, ax=ax)
    cbar.set_label("Normalised intensity  (0 = coldest, 255 = hottest)")
    ax.set_xlabel("Column  (0 \u2013 31)")
    ax.set_ylabel("Row  (0 \u2013 23)")
    subtitle = ax.set_title("Waiting for first frame \u2026", fontsize=10)
    fig.tight_layout()
    plt.show(block=False)

    # ── GUI update loop — runs on the main thread, never blocks asyncio. ─────
    while plt.fignum_exists(fig.number) and not stop_event.is_set():
        with _lock:
            frame = _last_frame.copy()
            t_min_local = _t_min
            t_max_local = _t_max

        img_plot.set_data(frame)
        subtitle.set_text(
            f"min {t_min_local:.1f} \u00b0C    "
            f"max {t_max_local:.1f} \u00b0C    "
            f"span {t_max_local - t_min_local:.1f} \u00b0C"
        )
        fig.canvas.draw_idle()
        fig.canvas.flush_events()
        time.sleep(0.2)

    stop_event.set()
    ble_thread.join(timeout=5.0)

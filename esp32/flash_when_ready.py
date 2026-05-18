#!/usr/bin/env python3

import glob
import subprocess
import time
import sys

PORT_PATTERNS = ["/dev/ttyACM*", "/dev/ttyUSB*"]

IDF_PYTHON = "/home/mm/.espressif/tools/python/v6.0/venv/bin/python"
IDF_TOOL = "/home/mm/.espressif/v6.0/esp-idf/tools/idf.py"


def find_port():
    for pattern in PORT_PATTERNS:
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


print("Waiting for ESP32 serial port... Press BUT1 / reconnect USB.")

while True:
    port = find_port()

    if port:
        print(f"Found {port}, flashing now...")

        cmd = [
            IDF_PYTHON,
            IDF_TOOL,
            "-p", port,
            "flash"
        ]

        result = subprocess.run(cmd)

        if result.returncode == 0:
            print("Flash successful.")
            sys.exit(0)

        print("Flash failed, waiting for port again...")

    time.sleep(0.2)
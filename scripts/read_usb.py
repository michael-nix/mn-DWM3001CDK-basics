#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyserial",
# ]
# ///

import sys
import serial
import serial.tools.list_ports

BAUD_RATE = 115200

TARGET_VID = 0x2FE3  # default value
TARGET_PID = 0x100  # default value

def find_port_by_vid_pid(vid=TARGET_VID, pid=TARGET_PID):
    """Return the first port matching the given VID/PID."""

    for port_info in serial.tools.list_ports.comports():
        if port_info.vid == vid and port_info.pid == pid:
            print(f"Found matching device: {port_info.device} "
                  f"(VID=0x{vid:04X}, PID=0x{pid:04X})")
            
            return port_info.device
    return None

if __name__ == "__main__":
    port = find_port_by_vid_pid()
    if port is None:
        print("USB port with given VID / PID pair not found.")

        sys.exit(1)

    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
        print(f"Reading from {ser.port} at {ser.baudrate} baud...")
        print("Press Ctrl+C to stop.\n")

        while True:
            # Example log to match on:
            # [00:01:41.302,856] <inf> dw3000_thread: 50563658c4f8c40d,1.596917
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            data_strings = line.split(" ")
            if not data_strings:
                continue

            data_strings = data_strings[-1].split(",")
            if len(data_strings) is 2:
                print("Device ID: " + data_strings[0], "-- Range: " + data_strings[1])

    except serial.SerialException as e:
        print(f"Serial error: {e}")

        sys.exit(1)

    except KeyboardInterrupt:
        print("\nExiting.")

        ser.close()

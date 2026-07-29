#!/usr/bin/env python3
"""Xiaomiao ESP32 serial console — configurable device path."""

import argparse
import sys
import serial
import serial.tools.list_ports
import base64
import struct

def list_ports():
    """List available serial ports."""
    print("Available serial ports:")
    for port in serial.tools.list_ports.comports():
        print(f"  {port.device} - {port.description}")

def auto_detect_port():
    """Auto-detect likely ESP32 port."""
    ports = serial.tools.list_ports.comports()
    # Prefer USB CDC (GD32 bridge) or CP2102
    for p in ports:
        if 'ACM' in p.device or 'USB' in p.device:
            return p.device
    return ports[0].device if ports else None

def send_command(port, baud, cmd, timeout=5):
    """Send command and read response."""
    with serial.Serial(port, baud, timeout=timeout) as ser:
        ser.write(f"{cmd}\n".encode())
        # Read until prompt or timeout
        response = b""
        while True:
            chunk = ser.read(1024)
            if not chunk:
                break
            response += chunk
            if b">>>" in chunk or b"$" in chunk:
                break
        return response.decode('utf-8', errors='ignore')

def capture_shot(port, baud, output_path):
    """Capture framebuffer shot from device."""
    print(f"Capturing shot from {port} @ {baud}...")
    response = send_command(port, baud, "shot")

    # Parse base64 data from response
    # Format: "SHOT: <base64_data>"
    lines = response.split('\n')
    b64_data = None
    for line in lines:
        if line.startswith("SHOT:"):
            b64_data = line.split("SHOT:")[1].strip()
            break

    if not b64_data:
        print("Error: No shot data received")
        print("Response:", response)
        return False

    # Decode: 160*128*2 = 40960 bytes of RGB565 raw data
    raw = base64.b64decode(b64_data)
    if len(raw) != 160 * 128 * 2:
        print(f"Warning: Expected {160*128*2} bytes, got {len(raw)}")

    # Save raw
    with open(output_path, 'wb') as f:
        f.write(raw)

    print(f"Shot saved to {output_path} ({len(raw)} bytes)")
    return True

def main():
    parser = argparse.ArgumentParser(description='Xiaomiao ESP32 serial console')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (auto-detect if omitted)')
    parser.add_argument('--baud', '-b', default=460800, type=int,
                        help='Baud rate (default: 460800)')
    parser.add_argument('--list', '-l', action='store_true',
                        help='List available ports')
    parser.add_argument('command', nargs='?', default=None,
                        help='Command to send (e.g., "shot", "state", "fps")')
    parser.add_argument('--output', '-o', default='shot.raw',
                        help='Output file for shot command')

    args = parser.parse_args()

    if args.list:
        list_ports()
        return 0

    port = args.port
    if not port:
        port = auto_detect_port()
        if not port:
            print("Error: No serial port found. Use --list to see available ports.")
            return 1
        print(f"Auto-detected port: {port}")

    if not args.command:
        print("Error: No command specified. Use --help for usage.")
        return 1

    if args.command == 'shot':
        return 0 if capture_shot(port, args.baud, args.output) else 1
    else:
        response = send_command(port, args.baud, args.command)
        print(response)
        return 0

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""
Simple HTTP server for OTA testing
Usage: python3 serve.py
"""
import http.server
import socketserver
import os

PORT = 8080
FIRMWARE_VERSION = "1.1.0"

# Required files:
# - firmware.bin: the new firmware (copy from .pio/build/esp32-s3-devkitc-1/firmware.bin)
# - version.txt: the new version

def setup_files():
    with open("version.txt", "w") as f:
        f.write(FIRMWARE_VERSION)
    print(f"version.txt set to: {FIRMWARE_VERSION}")
    
    if not os.path.exists("firmware.bin"):
        print("⚠️  firmware.bin not found!")
        print("   Copy from: .pio/build/esp32-s3-devkitc-1/firmware.bin")

if __name__ == "__main__":
    setup_files()
    
    handler = http.server.SimpleHTTPRequestHandler
    with socketserver.TCPServer(("", PORT), handler) as httpd:
        print(f"✅ OTA Server running on port {PORT}")
        print(f"   Version: {FIRMWARE_VERSION}")
        print(f"   Serving: {os.getcwd()}")
        httpd.serve_forever()
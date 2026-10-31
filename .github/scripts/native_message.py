"""Sends one native-messaging request to orbit-native-host, the way a browser does, and
prints the reply. Used by CI to check an installed Orbit end to end.

  python native_message.py <path to orbit-native-host> '<json request>'
"""
import json
import struct
import subprocess
import sys

host, request = sys.argv[1], json.dumps(json.loads(sys.argv[2])).encode()
out = subprocess.run([host], input=struct.pack("<I", len(request)) + request,
                     capture_output=True, timeout=60).stdout
if len(out) < 4:
    sys.exit("no reply from the native host")
(length,) = struct.unpack("<I", out[:4])
print(out[4:4 + length].decode())

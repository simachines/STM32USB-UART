import serial, time, sys

PORT = 'COM11'
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=0.5)
time.sleep(0.5)

# Flush any pending data
s.reset_input_buffer()

# Send RST to reset the ESP32 for a clean boot
print("Sending RST...")
s.write(b'RST\r\n')
time.sleep(0.3)

# Read for N seconds
duration = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
end = time.time() + duration
data = b''
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        data += chunk

s.close()
print("=== Captured %d bytes ===" % len(data))
print(data.decode('utf-8', errors='replace'))
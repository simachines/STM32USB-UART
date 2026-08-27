#include <Arduino.h>

// ESP32-C3 echo test firmware.
// Waits for a specific trigger message from the STM32 bridge, then blasts
// back a known binary pattern (mimicking the bootloader's binary response).
// This isolates whether the STM32 bridge's RX or TX path is the problem.
//
// Trigger: the 4-byte sequence 0xDE 0xAD 0xBE 0xEF
// Response: 64 bytes of a known pattern (0x00..0x3F), sent in one burst.

#define TRIGGER_LEN 4
const uint8_t trigger[TRIGGER_LEN] = {0xDE, 0xAD, 0xBE, 0xEF};

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32-C3 echo test ready. Send DE AD BE EF to trigger.");
}

void loop()
{
  // Check for the trigger sequence
  static uint8_t match = 0;
  while (Serial.available())
  {
    uint8_t b = Serial.read();
    if (b == trigger[match])
    {
      match++;
      if (match == TRIGGER_LEN)
      {
        match = 0;
        // Blast back 64 bytes of a known binary pattern IMMEDIATELY
        // (no flush, mimicking the bootloader's fast response)
        uint8_t buf[64];
        for (int i = 0; i < 64; i++)
        {
          buf[i] = (uint8_t)i;  // 0x00, 0x01, ..., 0x3F
        }
        Serial.write(buf, 64);
      }
    }
    else
    {
      match = (b == trigger[0]) ? 1 : 0;
    }
  }
}
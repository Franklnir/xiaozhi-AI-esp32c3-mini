# ESP32-C3 with INMP441 + MAX98357A + OLED

A XiaoZhi AI board for ESP32-C3 using:
- **Microphone**: INMP441 (I2S)
- **Speaker**: MAX98357A (I2S)
- **Display**: SSD1306 OLED 128x64 (I2C)
- **Buttons**: GPIO3 push-to-talk, GPIO2 hands-free toggle/reset

## Wiring

| ESP32-C3 Pin | Device | Function |
|--------------|--------|----------|
| GPIO 5 | INMP441 + MAX98357A | BCLK (shared) |
| GPIO 6 | INMP441 + MAX98357A | WS/LRC (shared) |
| GPIO 4 | INMP441 | SD (Mic Data) |
| GPIO 7 | MAX98357A | DIN (Speaker Data) |
| GPIO 3 | Button | Push-to-talk + WiFi config at boot |
| GPIO 2 | Button (optional) | Hands-free toggle (click), Reset SSID (hold 5s) |
| GPIO 8 | SSD1306 OLED | SDA |
| GPIO 9 | SSD1306 OLED | SCL |
| 5V | MAX98357A | VIN |
| 3V3 | INMP441, SSD1306 | VDD |
| GND | All | GND |

## Usage

1. **First boot**: Click GPIO3 button to enter WiFi config mode
2. **Push-to-talk mode (GPIO3)**: Hold to talk, release to stop (original function unchanged)
3. **Hands-free mode (default ON)**: Just speak without pressing; after you stop talking and silence is detected, XiaoZhi responds
4. **Auto low power in hands-free**: If no voice is detected for 12 seconds, device closes audio channel and returns to low-power standby
5. **Wake by voice command**: In standby, say wake word (default C3 model: `Hi Jason`) to wake and start listening again
   Note: ESP32-C3 built-in wake word models do not provide `Hallo Alexa`.
6. **GPIO2 toggle**: Click GPIO2 to switch hands-free ON/OFF
   OFF means fully disabled: no auto listen and no wake-by-voice.
7. **Reset WiFi credentials**: Hold GPIO2 for 5 seconds to clear saved SSID/password and reboot

## Local Song Command

- Voice command keywords: `nyanyi`, `putar lagu`, `sing`, `play song`
- Stop command while song is playing: `stop`, `berhenti`, `hentikan`, `pause`
- Song selection:
  - default: `song1`
  - contains `dua` / `2` / `two`: `song2`
  - contains `tiga` / `3` / `three`: `song3`
- GPIO3 push-to-talk can also interrupt/stop local song playback.
- File lookup order per song:
  - `/spiffs/songX.ogg`
  - `/spiffs/songs/songX.ogg`
  - `songX.ogg` (assets partition)
  - `songs/songX.ogg` (assets partition)
  - `music/songX.ogg` (assets partition)

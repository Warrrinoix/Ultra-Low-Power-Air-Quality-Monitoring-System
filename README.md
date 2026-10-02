# 🌬️ Ultra-Low-Power Air Quality Monitoring System

> **A battery-powered desk device built to simultaneously display indoor and outdoor air quality on a low-power e-paper interface.** 

The system uses an ESP32 microcontroller to fetch local outdoor pollution data over Wi-Fi while reading real-time indoor air conditions via on-board sensors.

---

## 📖 Overview
Most conventional weather apps provide a single Air Quality Index (AQI) for an entire city, omitting crucial indoor environmental data. This project bridges that gap by offering side-by-side indoor and outdoor metrics. 

The system relies on an ENS160 gas sensor for indoor Total Volatile Organic Compounds (TVOC) and equivalent CO2 (eCO2), an AHT21 sensor for temperature and humidity, and Open-Meteo APIs for outdoor PM2.5, PM10, and weather data. The outdoor PM2.5 is converted to the US EPA AQI standard using the updated 2024 breakpoints. The system operates on a 1.54-inch e-paper display, which retains its image even when power is removed, allowing the ESP32 to remain in deep sleep between one-minute updates.

## ✨ Features
* 📊 **Dual Air Quality Monitoring:** Displays indoor AQI (UBA 1-5 scale), eCO2, and TVOC alongside outdoor US EPA AQI (PM2.5 and PM10).
* 🌍 **Auto-Location & Weather:** Uses `ip-api.com` to determine the user's location based on their public IP, then fetches local weather and particle data from Open-Meteo.
* ⏰ **Timekeeping without RTC Module:** Synchronizes time via NTP servers using the ESP32's internal RTC, surviving deep sleep cycles.
* 📱 **Six-Screen Interface:** Navigate between Home, Indoor Air, Outdoor & Weather, Calendar, Alarm, and Location screens using a single physical push button[cite: 1].
* 🔔 **Built-in Alarm:** An active buzzer functions as a programmable daily alarm[cite: 1].
* 🦖 **Fallback Mini-Game:** Includes a turn-based "Dino Run" game that activates if the Wi-Fi connection fails or via a long press on the Home screen[cite: 1].

## 🧰 Hardware Requirements
| Component | Details |
| :--- | :--- |
| 🧠 **Microcontroller** | ESP32 DevKit (38-pin)[cite: 1] |
| 🌡️ **Sensors** | ENS160 + AHT21 integrated sensor board[cite: 1] |
| 📺 **Display** | Waveshare 1.54-inch e-Paper Module (200x200 resolution, SPI)[cite: 1] |
| 🔋 **Power Supply** | 12 V 2200 mAh LiPo battery pack[cite: 1] |
| ⚡ **Voltage Regulation** | LM2596 buck converter module (Must be tuned to 5.0 V)[cite: 1] |
| 🎛️ **Peripherals** | Push button, Active buzzer, Battery level indicator module, Power switch[cite: 1] |
| 🔌 **Capacitor** | 1000 µF bulk decoupling capacitor[cite: 1] |

## 🔌 Pin Configuration
The system uses standard I2C for the sensors and SPI for the e-paper display[cite: 1]. 

| ESP32 Pin | Component Connection | Function |
| :--- | :--- | :--- |
| `5V (VIN)` | LM2596 Buck Converter | 5.0 V Power Supply Input[cite: 1] |
| `3V3` | E-paper `VCC`, Sensor `VIN`, Capacitor `+` | 3.3 V Output for modules[cite: 1] |
| `GPIO23` | E-paper `DIN` | SPI MOSI[cite: 1] |
| `GPIO18` | E-paper `CLK` | SPI SCK[cite: 1] |
| `GPIO5` | E-paper `CS` | SPI Chip Select[cite: 1] |
| `GPIO17` | E-paper `DC` | Data / Command[cite: 1] |
| `GPIO16` | E-paper `RST` | Reset[cite: 1] |
| `GPIO4` | E-paper `BUSY` | Busy flag[cite: 1] |
| `GPIO21` | ENS160+AHT21 `SDA` | I2C Data[cite: 1] |
| `GPIO22` | ENS160+AHT21 `SCL` | I2C Clock[cite: 1] |
| `GPIO27` | Push Button | User Input (RTC-capable for deep sleep wake)[cite: 1] |
| `GPIO26` | Active Buzzer | Alarm Output[cite: 1] |
| `GND` | All Modules / Peripherals | Common Ground[cite: 1] |

*> Note: The push button uses the ESP32's internal pull-up resistor; no external resistor is required.*[cite: 1]

## 💻 Software Setup
1. Open the `.ino` firmware file in the Arduino IDE[cite: 1].
2. Install the required libraries via the Arduino Library Manager:
   * `WiFi`[cite: 1]
   * `HTTPClient`[cite: 1]
   * `ArduinoJson`[cite: 1]
   * `Wire`[cite: 1]
   * `SparkFun_ENS160`[cite: 1]
   * `Adafruit_AHTX0`[cite: 1]
   * `GxEPD2` (for the e-paper display)[cite: 1]
3. In the source code, configure your local network credentials[cite: 1]:
   ```cpp
   const char* ssid     = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";

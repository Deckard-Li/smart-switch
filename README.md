# Smart Switch for Speaker

An ESP-01S based smart relay controller designed for speakers, integrating WiFi captive portal configuration, MQTT control, and local ARP-based MAC address presence detection.

## Features

- **Captive Portal Configuration**: On first boot or when WiFi fails, the device acts as a WiFi Access Point (`SmartSwitch-Setup`). You can configure the WiFi credentials and MQTT server settings via a web browser at `192.168.4.1`.
- **MQTT Control**: Send commands to turn the relay on/off and enable/disable the presence scanning feature.
- **Ping-based Presence Detection**: Continuously monitors the local network for a specific device's IP address. If the device connects to the network and responds to ping (e.g., your smartphone or TV), the smart switch automatically turns on the relay. If the device disconnects or shuts down, the relay turns off. This method is highly reliable and avoids issues with devices that keep their network cards powered on (Wake-on-LAN) even when shut down.
- **EEPROM Persistence**: All settings (WiFi, MQTT server, target MAC address, and device state) are safely stored in flash memory, meaning they survive power cycles.
- **Compact Hardware**: Built for the ESP-01S and standard 5V/3.3V relay modules, utilizing GPIO0 for the relay control.

## Hardware Requirements

- **ESP-01S** (or standard ESP8266 module with 1MB+ flash)
- **ESP-01 Relay Module** (uses GPIO0 to trigger the relay)
- 5V power supply

## Installation & Flashing

This project uses [PlatformIO](https://platformio.org/).

1. Clone this repository.
2. Open the project in VSCode with the PlatformIO extension installed.
3. Build and upload the firmware to your ESP-01S.
   *(Note: The device polls the target IP address every 30 seconds to ensure the relay turns off promptly after the target device goes offline).*

## Initial Setup

1. Power on the device. Since there are no saved credentials, it will start an access point named **`SmartSwitch-Setup`**.
2. Connect to this WiFi network from your phone or PC.
3. A captive portal should automatically open. If not, open a browser and go to `http://192.168.4.1`.
4. Enter your home WiFi SSID, Password, and your MQTT Broker's IP address.
5. Click **Save & Restart**. The switch will connect to your WiFi and MQTT broker.

## MQTT Topics

The device listens and publishes to the following topics (defaults configured in `include/config.h`):

### Relay Control
- **Command Topic**: `home/smart-switch/relay/command`
  - Send `ON` to turn the relay on.
  - Send `OFF` to turn the relay off (this acts as a master kill switch and suspends MAC scanning).
- **State Topic**: `home/smart-switch/relay/state`
  - Publishes `ON` or `OFF`.

### Presence Scan Control
- **Command Topic**: `home/smart-switch/scan/command`
  - Send `ON` to enable ping scanning.
  - Send `OFF` to disable ping scanning.
- **State Topic**: `home/smart-switch/scan/state`
  - Publishes `ON` or `OFF`.

### Target IP Address Configuration
- **Command Topic**: `home/smart-switch/ip/command`
  - Send the static IP address of the device you want to track (e.g., `192.168.1.100`).
- **State Topic**: `home/smart-switch/ip/state`
  - Publishes the currently tracked IP address.

## How the Scanner Works

When `relay/command` is set to `ON` and the scan is enabled, the device sends a single ICMP echo request (ping) to the target IP address every 30 seconds. If the device responds, the relay turns ON. If the ping times out or fails (e.g., the device is shut down or goes offline), the relay turns OFF. This method avoids the need to disconnect from WiFi, keeping the MQTT connection completely stable, and is immune to devices keeping their network cards active for Wake-on-LAN purposes.

## Resetting the Device

If you ever need to change the WiFi credentials or if the device gets stuck:
1. Turn off your WiFi router temporarily.
2. Restart the ESP-01S.
3. After 15 seconds of failing to connect, it will fall back to AP mode and broadcast the `SmartSwitch-Setup` network again.

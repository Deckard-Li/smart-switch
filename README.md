# Smart Switch for Speaker

An ESP-01S based smart relay controller designed for speakers, integrating WiFi captive portal configuration, MQTT control, and local ARP-based MAC address presence detection.

## Features

- **Captive Portal Configuration**: On first boot or when WiFi fails, the device acts as a WiFi Access Point (`SmartSwitch-Setup`). You can configure the WiFi credentials and MQTT server settings via a web browser at `192.168.4.1`.
- **MQTT Control**: Send commands to turn the relay on/off and enable/disable the presence scanning feature.
- **MAC-to-IP Ping Presence Detection**: The switch tracks a specific device's MAC address. It periodically broadcasts ARP requests to discover the device's current IP address, and then uses a highly reliable ICMP ping to check if the device is active. If the device responds to the ping (e.g., your smartphone or TV is turned on), the smart switch automatically turns on the relay. If the ping fails (e.g., the device is shut down or goes offline), the relay turns off. This hybrid method gives you the reliability of pinging (bypassing Wake-on-LAN false positives) without requiring you to configure static IP addresses on your router!
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
   *(Note: The device polls the target MAC/IP address every 30 seconds to ensure the relay turns off promptly after the target device goes offline).*

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

### Target MAC Address Configuration
- **Command Topic**: `home/smart-switch/mac/command`
  - Send the MAC address of the device you want to track (e.g., `AA:BB:CC:DD:EE:FF`).
- **State Topic**: `home/smart-switch/mac/state`
  - Publishes the currently tracked MAC address.

## How the Scanner Works

When `relay/command` is set to `ON` and the scan is enabled, the device first sends lightweight ARP requests across your `/24` subnet. It then checks the lwIP ARP cache to discover the current IP address of your target MAC.
Once the IP is known, it sends a single ICMP echo request (ping) to that IP every 30 seconds. If the device responds, the relay turns ON. If the ping times out or fails (e.g., the device is shut down or goes offline), the relay turns OFF and the IP is discarded (so the next cycle will start an ARP sweep again to find the new IP if it reconnects).
This hybrid method gives you the reliability of Ping without needing to set static IP addresses on your router, and is immune to devices keeping their network cards active for Wake-on-LAN purposes.

## Resetting the Device

If you ever need to change the WiFi credentials or if the device gets stuck:
1. Turn off your WiFi router temporarily.
2. Restart the ESP-01S.
3. After 15 seconds of failing to connect, it will fall back to AP mode and broadcast the `SmartSwitch-Setup` network again.

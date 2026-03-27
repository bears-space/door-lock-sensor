
# Door Lock Sensor

Checks if the door is locked/unlocked with a keyboard blueswitch that is pressed by the lock bolt.
Uses an ESP8266 (ESP-12F module) which connects to WiFi and publishes an MQTT event to Home Assistant on activation and implements a debounce timer to avoid spamming MQTT events.

# Installing the ESP8266 SDK and Toolchain

- Requires Python 3.7, I recommend creating a virtual environment
- [installation guide](https://github.com/espressif/ESP8266_RTOS_SDK)

There is a bug in the SDK that leads to ncurses library errors on Arch Linux, here's the [fix](https://gist.github.com/fiffy326/070dfd4e21a311140cdedc5dfc20c25e)

# Setup and Configuration

Use `make menuconfig` and fill in the required settings under the `Door Lock Sensor Config` Tab. (wifi ssid, password, mqtt broker, etc.)

# Build and Flash

Use `make -jn all flash monitor` (fill in `n` for the number of CPU cores), to compile, flash and monitor the program.
If the bootloader is flashed once you can use `make app-flash monitor` to flash without flashing the bootloader.

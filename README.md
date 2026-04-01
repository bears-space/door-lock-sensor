# Door Lock Sensor

This project runs on an ESP8266 (ESP-12F) with the ESP8266 RTOS SDK v3.4.
It reads a lock sensor on `D2 / GPIO4`, drives the onboard blue LED on `GPIO2`, connects to Wi-Fi, and publishes state to Home Assistant over MQTT.

It also supports a second button on `D3 / GPIO0` that arms a temporary "coffee break" mode.

# Hardware

- `D2 / GPIO4`: door lock sensor input, pulled up internally, active low
- `D3 / GPIO0`: coffee break button input, pulled up internally, active low
- `GPIO2`: onboard blue LED output

Notes:

- The door sensor is interpreted as `LOW = unlocked` and `HIGH = locked`.
- The blue LED is on during normal unlocked state.
- `GPIO0` is a boot strap pin on the ESP8266, so the button on `D3` must not be held low while the board is booting or resetting.

# Behaviour

## Normal mode

- The door sensor uses a stable-state debounce with the time configured by `DEBOUNCE_LIMIT_MS`.
- A normal unlock publishes:
  - door sensor `ON`
  - coffeebrake sensor `OFF`
- A normal lock publishes:
  - door sensor `OFF`
  - coffeebrake sensor `OFF`
- The LED mirrors the normal door state:
  - on when unlocked
  - off when locked

The Home Assistant door entity uses `device_class: lock`, so:

- `ON` means unlocked
- `OFF` means locked

## Coffee break mode

Coffee break can only be armed while the door is currently unlocked.

1. Press the button once while the blue LED is on.
2. The LED starts flashing rapidly.
3. The system is now armed for 5 minutes.

While armed:

- If nothing else happens for 5 minutes, the mode expires and the LED returns to normal.
- If the button is held for 5 seconds, the mode is cancelled immediately and the LED returns to normal.
- If the door locks during that 5 minute window:
  - `coffeebrake` is published as `ON`
  - no door locked MQTT update is sent
  - the LED switches to a slow flash

Once coffee break is active:

- If the door is unlocked again before timeout:
  - `coffeebrake` is published as `OFF`
  - no door unlocked MQTT update is sent
  - the system returns to normal mode
- If the door stays locked for 1.5 hours:
  - `coffeebrake` is published as `OFF`
  - the door sensor is published as locked (`OFF`)
  - the system returns to normal mode

# MQTT / Home Assistant

The firmware publishes three retained MQTT topics:

- door discovery topic
- coffeebrake discovery topic
- shared availability topic

And two retained state topics:

- door state topic
- coffeebrake state topic

On MQTT reconnect the ESP republishes:

- Home Assistant discovery for both entities
- availability as `online`
- the current runtime state

If the ESP drops off unexpectedly, the MQTT last will publishes availability `offline`.

# Configuration

Use `make menuconfig` and configure the values under `Door Lock Sensor Config`:

- Wi-Fi SSID and password
- MQTT broker URL, username, and password
- door discovery topic
- door state topic
- coffeebrake discovery topic
- coffeebrake state topic
- door debounce time in milliseconds

Also set the Xtensa toolchain path under `SDK Tool Configuration`.

Example toolchain prefix:

```bash
/home/user/esp/xtensa-lx106-elf/bin/xtensa-lx106-elf-
```

# How The Code Works

The firmware in [`main/main.c`](main/main.c) is split into a few small pieces:

- Wi-Fi setup and reconnect handling:
  - starts station mode
  - keeps event handlers registered
  - reconnects automatically after disconnects
- MQTT setup and reconnect handling:
  - starts the broker client
  - republishes discovery and current state after reconnect
  - publishes availability with a last-will topic
- Input processing:
  - polls both GPIO inputs every 20 ms
  - debounces the door sensor with the configured debounce time
  - debounces the coffee break button with a normal 50 ms debounce
- State machine:
  - `COFFEE_MODE_NORMAL`
  - `COFFEE_MODE_ARMED`
  - `COFFEE_MODE_ACTIVE`
- LED output:
  - steady on/off in normal mode
  - fast flashing while armed
  - slow flashing while coffee break is active

The main loop does four things repeatedly:

1. Read and debounce the door sensor.
2. Read and debounce the button.
3. Advance time-based coffee break transitions.
4. Update the LED pattern.

# Installing the ESP8266 SDK and Toolchain

- Requires Python 3.7
- Install the ESP8266 RTOS SDK: [installation guide](https://github.com/espressif/ESP8266_RTOS_SDK)
- Export the SDK path, for example:

```bash
export IDF_PATH=/home/user/esp/ESP8266_RTOS_SDK
```

There is a known ncurses issue on Arch Linux; this fix may help:
[ncurses fix](https://gist.github.com/fiffy326/070dfd4e21a311140cdedc5dfc20c25e)

# Build And Flash

Build, flash, and open the serial monitor with:

```bash
make -j4 all flash monitor
```

If the bootloader is already flashed, you can use:

```bash
make app-flash monitor
```

If your SDK and toolchain are not already exported in the shell, a working example looks like this:

```bash
PATH=/home/nick/.platformio/packages/toolchain-xtensa/bin:$PATH \
IDF_PATH=/home/nick/esp/ESP8266_RTOS_SDK \
make app-flash monitor
```

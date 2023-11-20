# ESPHome HDMI-CEC Component

Make your ESPHome devices speak the (machine) language of your living room with this native HDMI-CEC (Consumer Electronics Control) component!

## Features

- Native CEC 1.3a implementation
    - Implemented from scratch specifically for this component. No third-party CEC library.
    - Meant to be as simple, lightweight and easy-to-understand as possible
- Receive CEC commands
    - Acknowledgements handled automatically
    - Handle incoming messages with the `on_message` trigger
    - Interrupts-based receiver. No polling.
- Send CEC commands
    - Built-in `hdmi_cec.send` action

### To-do list

- Handle system messages like _"Get CEC Version"_ natively
- Handle Physical Address Queries and Discovery

## Installation

1. Wire your ESPhome device to an HDMI connector (e.g. an HDMI breakout that can be found on Amazon) as follows :
    - GPIO pin of your choice -> HDMI pin 13 (CEC)
    - GND -> HDMI pin 17 (DDC/CEC ground)
    - _Optional_: wire your board's 5V supply to HDMI pin 18. This is how your TV/switch on the other end knows something is connected.

    The CEC bus uses 3.3V logic, so it's perfectly safe for ESP32/ESP8266 devices (or any other microcontroller with 3.3V logic).

2. In your ESPhome configuration file, add this Git repository as an external component

```yaml
external_components:
  - source: github://Palakis/esphome-hdmi-cec
```

3. Setup the HDMI-CEC component

```yaml
hdmi_cec:
  pin: GPIO26 # Pick a GPIO pin that can do both input AND output
  address: 0x3 # 0x3 => "Tuner 1"
  on_message:
    # Handle CEC Version requests
    - opcode: 0x9F
      then:
        hdmi_cec.send:
          destination: !lambda return source;
          data: [0x9E, 0x04]
```

4. (optional) Use the `hdmi_cec.send` action in your ESPHome configuration

```yaml
button:
  - platform: template
    name: "Turn everything off"
    on_press:
      hdmi_cec.send:
        destination: 0xF # Broadcast
        data: [0x36] # Standby opcode
```

## Compatibility

- ESP32: ✅ **tested, works**
- ESP8266: ❔ _untested, but should work_
- RP2040: ❔ _untested_
- LibreTiny: ❔ _untested_

## Acknowledgements

- [johnboiles' `esphome-hdmi-cec` component](https://github.com/johnboiles/esphome-hdmi-cec) has been the inspiration and starting point of this project.

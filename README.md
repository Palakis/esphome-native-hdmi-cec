# ESPHome HDMI-CEC Component

Make your ESPHome devices speak the (machine) language of your living room with this native HDMI-CEC (Consumer Electronics Control) component!

## Features

- Native CEC 1.3a implementation
    - Implemented from scratch specifically for this component. No third-party CEC library used.
    - Meant to be as simple, lightweight and easy-to-understand as possible
    - Interrupts-based receiver (no polling at all). Handles low-level byte acknowledgements
- Receive CEC commands
    - Handle incoming messages with `on_message` triggers
      - Each trigger specified in `on_message` supports filtering based on source, destination, opcode and/or message contents
    - Built-in handlers for some of the system commands defined in the spec :
      - _"Get CEC Version"_
      - _"Give Device Power Status"_
      - _"Give OSD Name"_
- Send CEC commands
    - Built-in `hdmi_cec.send` action

### To-do list

- Automatic Physical Address Discovery through E-DDC
- Ability to override built-in command handlers

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
  # Pick a GPIO pin that can do both input AND output
  pin: GPIO26 # Required
  # The address can be anything you want. Use 0xF if you only want to listen to the bus and not act like a standard device
  address: 0xE # Required
  # Physical address of the device. In this case: 4.0.0.0 (HDMI4 on the TV)
  # DDC support is not yet implemented, so you'll have to set this manually.
  physical_address: 0x4000 # Required
  # The name that will we displayed in the list of devices on your TV/receiver
  osd_name: "my device" # Optional. Defaults to "esphome"
  # By default, promiscuous mode is disabled, so the component only handles directly-address messages (matching
  # the address configured above) and broadcast messages. Enabling promiscuous mode will make the component
  # listen for all messages (both in logs and the on_message triggers)
  promiscuous_mode: false # Optional. Defaults to false
  # List of triggers to handle specific commands. Each trigger has the following optional filter parameters:
  # - "source": match messages coming from the specified address
  # - "destination": match messages meant for the specified address
  # - "opcode": match messages bearing the specified opcode
  # - "data": exact-match on message content
  # Actions called from these triggers is called with "source", "destination" and "data" as parameters
  on_message:
    - opcode: 0x36 # opcode for "Standby"
      then:
        logger.log: "Got Standby command"
    
    # Respond to "Menu Request" (not required, example purposes only)
    - opcode: 0x8D
      then:
        hdmi_cec.send:
          # both "destination" and "data" are templatable
          destination: !lambda return source;
          data: [0x8E, 0x01] # 0x01 => "Menu Deactivated"

```

4. (optional) Use the `hdmi_cec.send` action in your ESPHome configuration

```yaml
button:
  - platform: template
    name: "Turn everything off"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 0xF # Broadcast
        data: [0x36] # "Standby" opcode
```

## Compatibility

- ESP32: ✅ **tested, works**
- ESP8266: ❔ _untested, but should work_
- RP2040: ❔ _untested_
- LibreTiny: ❌ **not supported**
  - I don't have any LibreTiny device at hand. Feel free to run your own tests and report back your findings.

## Acknowledgements

- [johnboiles' `esphome-hdmi-cec` component](https://github.com/johnboiles/esphome-hdmi-cec) has been the inspiration and starting point of this project.

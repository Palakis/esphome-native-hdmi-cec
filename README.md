# ESPHome Native HDMI-CEC Component

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
  # By default, monitor mode is disabled, so the component can send messages and acknowledge incoming messages.
  # Enabling monitor mode lets the component act as a passive listener, disabling active manipulation of the CEC bus.
  monitor_mode: false # Optional. Defaults to false
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


```
button:
  - platform: template
    name: "Turn all HDMI devices off"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 0xF # Broadcast
        data: [0x36] # "Standby" opcode
  - platform: template
    name: "Turn TV on"
    on_press:
      hdmi_cec.send:
        source: 1 # can optionally be set, like if you want to spoof another device's address
        destination: 0
        data: [0x04]
  - platform: template
    name: "Turn TV off"
    on_press:
      hdmi_cec.send:
        source: 1 # can optionally be set, like if you want to spoof another device's address
        destination: 0
        data: [0x36]
  - platform: template
    name: "Volume up"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 0x5
        data: [0x44, 0x41]
  - platform: template
    name: "Volume down"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 0x5
        data: [0x44, 0x42]
  - platform: template
    name: "Mute"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 0x5
        data: [0x44, 0x43]
  - platform: template
    name: "Turn on Playback device 1"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x44, 0x6D]
  - platform: template
    name: "Turn off Playback device 1"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x36]
  - platform: template
    name: "Playback device 1 home button"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x44, 0x09]
  - platform: template
    name: "Playback device 1 select/ok"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x44, 0x00]
  - platform: template
    name: "Playback device 1 exit/back"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x44, 0x0D]
  - platform: template
    name: "Playback device 1 play/pause"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 4
        data: [0x44, 0x44]
  - platform: template
    name: "Turn on Playback device 2"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 8
        data: [0x44, 0x6D]
  - platform: template
    name: "Turn off Playback device 2"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 8
        data: [0x36]
  - platform: template
    name: "Playback device 2 play/pause"
    on_press:
      hdmi_cec.send:
        # "source" can optionally be set, like if you want to spoof another device's address
        destination: 8
        data: [0x44, 0x46]
```

5. (optional) Add Services for HomeAssistant

```
api
  ...
  services:
    - service: hdmi_cec_send
      variables:
        cec_destination: int
        cec_data: int[]
      then:
        - hdmi_cec.send:
            destination: !lambda "return static_cast<unsigned char>(cec_destination);"
            data: !lambda "std::vector<unsigned char> charVector; for (int i : cec_data) { charVector.push_back(static_cast<unsigned char>(i)); } return charVector;"
```

6. (optional) Send data via MQTT in CEC-o-matic format

```
mqtt:
  broker: 'homeassistantaddress'
  username: 'mqtt_user'
  password: 'mqtt_password'
  discovery: false # if you only want your own MQTT topics

hdmi_cec:
  pin: GPIO26
  address: 0xE
  physical_address: 0x4000
  promiscuous_mode: true 
  on_message:
    # No source, destination or opcode set => catch all messages
    - then:
        mqtt.publish:
          topic: cec_messages
          payload: !lambda |-
            std::vector<uint8_t> full_frame;
            full_frame.push_back((source << 4) | (destination & 0xF));
            full_frame.insert(full_frame.end(), data.begin(), data.end());
            return hdmi_cec::bytes_to_string(full_frame);       
```

8. (optional) Add message decoder and text sensors

```
  on_message:
  - then:
      - lambda: |-
          // — Build raw frame string —
          std::vector<uint8_t> frame;
          frame.push_back((source << 4) | (destination & 0xF));
          frame.insert(frame.end(), data.begin(), data.end());
          std::string raw = hdmi_cec::bytes_to_string(frame);
          id(cec_raw_message).publish_state(raw);

          // — Helper lambdas —
          auto formatWithPeriods = [&](uint16_t num) {
            std::string s = std::to_string(num), out;
            for (char c : s) { out += c; out += '.'; }
            if (!out.empty()) out.pop_back();
            return out;
          };
          auto toHex = [&](uint8_t b) {
            char buf[3]; sprintf(buf, "%02X", b);
            return std::string(buf);
          };
          auto getDeviceName = [&](uint8_t nib) {
            static const char* names[] = {
              "TV","Recorder 1","Recorder 2","Tuner 1","Playback 1",
              "Audio system","Tuner 2","Tuner 3","Playback 2","Recorder 3",
              "Tuner 4","Playback 3","Reserved 1","Reserved 2","Free Use","Broadcast"
            };
            return std::string(names[nib & 0xF]);
          };
          // apply your custom names
          auto applyCustom = [&](const std::string &n) {
            if (n == "Playback 1")       return std::string("Apple TV");
            if (n == "Playback 2")       return std::string("PlayStation");
            if (n == "Audio system")     return std::string("Denon AVR");
            return n;
          };

          // — Build device prefix —
          uint8_t first = frame[0], src = first >> 4, dst = first & 0xF;
          std::string dev_src = applyCustom(getDeviceName(src));
          std::string dev_dst = applyCustom(getDeviceName(dst));
          std::string prefix = dev_src
            + (dst == 0xF
                ? " broadcasting:"
                : " to " + dev_dst + ":");

          // — Parse opcode & args —
          uint8_t opcode = frame.size()>1 ? frame[1] : 0x00;
          uint8_t b2     = frame.size()>2 ? frame[2] : 0x00;
          uint8_t b3     = frame.size()>3 ? frame[3] : 0x00;
          uint16_t phys  = (b2 << 8) | b3;
          int volume     = static_cast<int>(b2) - 2;
          std::string action;

          // — Full translation switch —
          switch (opcode) {
              case 0x0D:
              case 0x04:
                action = "I am the active source";
                break;
              case 0xA0:
                action = "Routing information";
                break;
              case 0x1A:
                action = std::string("My state is")
                  + (b2 == 0x01 ? " (On)"
                  : b2 == 0x02 ? " (Off)" : "");
                break;
              case 0x7A:
                action = "Audio volume (" + std::to_string(volume) + ")";
                break;
              case 0x7D:
                action = "Request audio mode status";
                break;
              case 0x7E:
                action = std::string("Audio mode")
                  + (b2 == 0x01 ? " (On)"
                  : b2 == 0x00 ? " (Off)" : "");
                break;
              case 0x8C:
                action = "Requesting vendor ID";
                break;
              case 0x8E:
                action = "CEC routing control command";
                break;
              case 0x8F:
                action = "Requesting power state";
                break;
              case 0x9D:
                action = "Inactive source with physical address ("
                  + formatWithPeriods(phys) + ")";
                break;
              case 0x9E:
                action = "Reporting CEC version (not translated)";
                break;
              case 0x9F:
                action = "Requesting CEC version";
                break;
              case 0x36:
                action = "Standby";
                break;
              case 0x44:
                action = "Button pressed"
                  + (b2 == 0x00 ? " (Select)"
                  : b2 == 0x01 ? " (Up)"
                  : b2 == 0x02 ? " (Down)"
                  : b2 == 0x03 ? " (Left)"
                  : b2 == 0x04 ? " (Right)"
                  : b2 == 0x41 ? " (Volume up)"
                  : b2 == 0x42 ? " (Volume down)"
                  : b2 == 0x46 ? " (Play/Pause)"
                  : " (Unknown: " + toHex(b2) + toHex(b3) + ")");
                break;
              case 0x45:
                action = "Button released";
                break;
              case 0x47:
                action = "Set OSD display name (not translated)";
                break;
              case 0x70:
                action = "Set active audio source ("
                  + formatWithPeriods(phys) + ")";
                break;
              case 0x71:
                action = "Requesting system audio mode";
                break;
              case 0x72:
                action = std::string("Set system audio mode")
                  + (b2 == 0x01 ? " (On)"
                  : b2 == 0x00 ? " (Off)"
                  : " (Unknown: " + toHex(b2) + ")");
                break;
              case 0x82:
                action = "Set active source ("
                  + formatWithPeriods(phys) + ")";
                break;
              case 0x83:
                action = "What is your physical address?";
                break;
              case 0x84:
                action = "My physical address is ("
                  + formatWithPeriods(phys) + ")";
                break;
              case 0x85:
                action = "Requesting active source";
                break;
              case 0x87:
                action = "Reporting device vendor ID (not translated)";
                break;
              case 0x89:
                action = "Vendor command (not translated)";
                break;
              case 0x90:
                action = "Reporting power status"
                  + (b2 == 0x00 ? " (Power On)"
                  : b2 == 0x01 ? " (Standby)"
                  : b2 == 0x02 ? " (In transition from Standby to On)"
                  : b2 == 0x03 ? " (Power Off)"
                  : " (Unknown: " + toHex(b2) + ")");
                break;
              default:
                action = "Unknown action (0x" + toHex(opcode) + ")";
                break;
            }

          // — Publish the human-readable translation —
          std::string translated = prefix + " " + action;
          id(cec_translated_message).publish_state(translated);


text_sensor:
  - platform: template
    name: "HDMI CEC Raw Message"
    id: cec_raw_message
    update_interval: never

  - platform: template
    name: "HDMI CEC Translated Message"
    id: cec_translated_message
    update_interval: never
```




## Compatibility

- ESP32: ✅ **tested, works**
- ESP8266: ✅ **tested, works**
- RP2040: ✅ **tested, works**
- LibreTiny: ❌ **not supported**
  - I don't have any LibreTiny device at hand. Feel free to run your own tests and report back your findings.

## Acknowledgements

- [johnboiles' `esphome-hdmi-cec` component](https://github.com/johnboiles/esphome-hdmi-cec) has been the inspiration and starting point of this project.

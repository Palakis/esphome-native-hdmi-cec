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

## 🚀 Getting Started (Quick Overview)

### 🪰 Step 1: Connect the hardware

Connect the microcontroller to an HDMI connector (HDMI connectors and breakout boards can be found on Amazon and AliExpress)

| [HDMI Pin](https://en.wikipedia.org/wiki/HDMI)     | Connect to | Microcontroller pin                           |
| -------- | --------- | --------------------------- |
| 13 (CEC Data Line)  | => |    Any input/output GPIO (e.g., GPIO26) |
| 17 (CEC Ground)    | => | Ground                                  |
| 18  (+5V (optional)) | => | 5V                                   |

> CEC uses 3.3V logic – safe for ESP32/ESP8266 (or any other microcontroller with 3.3V logic).


### 🧱 Step 2: Set up ESPHome

* Start by creating your device using **ESPHome Device Builder** (e.g., via Home Assistant’s ESPHome Add-on or ESPHome Web).
* Once your device is created, click **"Edit"** to access the YAML configuration.
* (If using an ESP32-C3, it’s recommended to use type: esp-idf)

---

### 📦 Step 3: Add the component

In your ESPhome YAML configuration, add this Git repository as an external component (e.g. below captive portal):

```yaml
external_components:
  - source: github://Palakis/esphome-hdmi-cec
```

---

### 🧠 Step 4: Basic HDMI-CEC Setup

Add the `hdmi_cec:` block:

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

```

You now have a functioning CEC receiver.

---

## ➕ Optional Features

All of the following are optional – include only what you need.

---

### 🔀 1. React to Incoming Messages

Add under `hdmi_cec:`:

```yaml
hdmi_cec:
  ...
  on_message:
    - opcode: 0x36  # "Standby"
      then:
        logger.log: "Received standby command"
    
    # Respond to "Menu Request" (not required, example purposes only)
    - opcode: 0x8D
      then:
        hdmi_cec.send:
          # both "destination" and "data" are templatable
          destination: !lambda return source;
          data: [0x8E, 0x01] # 0x01 => "Menu Deactivated"

```

You can filter by:

  * "source": match messages coming from the specified address
  * "destination": match messages meant for the specified address
  * "opcode": match messages bearing the specified opcode
  * "data": exact-match on message content

If no filter is set, you will catch all messages.

---

### 🔘 2. Add Template Buttons to Send CEC Commands

Add a `button:` section to create UI buttons:

```yaml
button:
  - platform: template
    name: "Turn TV Off"
    on_press:
      hdmi_cec.send:
        destination: 0
        data: [0x36]
```

> More button examples in the advanced EHPHome configuration example below.

---

### 🌐 3. Enable CEC Commands via Home Assistant Services

Under `api:`:

```yaml
api:
  services:
    - service: hdmi_cec_send
      variables:
        cec_destination: int
        cec_data: int[]
      then:
        - hdmi_cec.send:
            destination: !lambda "return static_cast<unsigned char>(cec_destination);"
            data: !lambda |-
              std::vector<unsigned char> vec;
              for (int i : cec_data) vec.push_back(static_cast<unsigned char>(i));
              return vec;
```

---

### ☁️ 4. Publish CEC Messages over MQTT ([CEC-O-MATIC](https://www.cec-o-matic.com/) format)

Under `mqtt:` and `hdmi_cec:`:

```yaml
mqtt:
  broker: '192.168.1.100' # insert IP or DNS of your own MQTT broker (e.g. the IP of your HA server)
  username: !secret mqtt_user # make sure your MQTT username is added to the secrets file in the ESPHome Add-on
  password: !secret mqtt_password # make sure your MQTT password is added to the secrets file in the ESPHome Add-on
  discovery: false # if you only want your own MQTT topics

hdmi_cec:
  ...
  promiscuous_mode: true
  on_message:
    - then:
        mqtt.publish:
          topic: cec_messages
          #Payload in CEC-O-Matic format
          payload: !lambda |-
            return hdmi_cec::Frame(source, destination, data).to_string(true);
```

---

### 🔍 5. Decode and Translate CEC Messages (Text Sensor + Decoder)

Create a readable message with device names and actions.

#### Add this under `hdmi_cec:`:

```yaml
hdmi_cec:
  ...
  on_message:
      
      #CEC message decoder (human-readable translation)
    - then:
        - lambda: |-
            std::string translated = hdmi_cec::Frame(source, destination, data).to_string();
            id(cec_translated_message).publish_state(translated);
```

#### And add two `text_sensor:` blocks (required):

```yaml
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
> If MQTT is enabled, the text sensor values (raw and translated) will also be sent via MQTT

---

### ✎6. Runtime configuration of the physical and logical address of HDMI

If you want the logical address and the physical address to be configurable, you can add input number fields.

```yaml
number:
  - platform: template
    optimistic: true
    entity_category: config
    name: HDMI Logical address
    step: 1
    min_value: 0x1 # Recording device 1
    max_value: 0xb # Playback device 3
    icon: "mdi:video-input-hdmi"
    restore_value: true
    on_value:
      - then:
          lambda: |-
            id(hdmicec)->set_address(x);
  - platform: template
    optimistic: true
    entity_category: config
    name: HDMI Physical address
    id: hdmi_phys_addr
    disabled_by_default: true
    step: 1
    min_value: 0x1000
    max_value: 0x4fff # Assuming up to 4 HDMI ports
    initial_value: 0x1001 # Avoid conflict with any existing device.
    mode: box
    icon: "mdi:hdmi-port"
    restore_value: true
    on_value:
      - lambda: |-
          id(hdmicec)->set_physical_address(x);

hdmi_cec:
  id: hdmicec
  ...
```
You may also consider adding 2 separate number fields - HDMI port (1-4) and adjustment (0-15) to calculate the physical address as  
`physical address = (port * 0x1000 + adjustment)` (0x1000-0x100f, 0x4000-0x400f)
The adjustment field is used to avoid conflicts with any existing device on those ports.
Note that the physical_address and address fields still need to be specified in the `hdmi_cec:` section.

---

## 🧪 Advanced Example (All Features Combined)

Here’s a full YAML snippet that includes all optional features together:

```yaml
esphome:
  name: hdmi-cec-bridge
  friendly_name: HDMI CEC Bridge

esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: esp-idf

# Enable logging
logger:

# Enable Home Assistant API
api:
  encryption:
    key: "..."
  
  services:
    - service: hdmi_cec_send
      variables:
        cec_destination: int
        cec_data: int[]
      then:
        - hdmi_cec.send:
            destination: !lambda "return static_cast<unsigned char>(cec_destination);"
            data: !lambda "std::vector<unsigned char> charVector; for (int i : cec_data) { charVector.push_back(static_cast<unsigned char>(i)); } return charVector;"

ota:
  - platform: esphome
    password: "..."

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

  # Enable fallback hotspot (captive portal) in case wifi connection fails
  ap:
    ssid: "HDMI CEC Fallback Hotspot"
    password: "..."

mqtt:
  broker: '192.168.1.100' # insert IP or DNS of your own MQTT broker (e.g. the IP of your HA server)
  username: !secret mqtt_user # make sure your MQTT username is added to the secrets file in the ESPHome Add-on
  password: !secret mqtt_password # make sure your MQTT password is added to the secrets file in the ESPHome Add-on
  discovery: false # if you only want your own MQTT topics

captive_portal:

external_components:
  - source: github://Palakis/esphome-hdmi-cec

hdmi_cec:
  # Pick a GPIO pin that can do both input AND output
  pin: GPIO10 # Required
  
  # The address can be anything you want. Use 0xF if you only want to listen to the bus and not act like a standard device
  address: 0xE # Required
  
  # Physical address of the device. In this case: 4.2.0.0 (The ESP32 is plugged into HDMI 2 on the receiver which is plugged into HDMI4 on the TV)
  # DDC support is not yet implemented, so you'll have to set this manually.
  physical_address: 0x4200 # Required
  
  # The name that will we displayed in the list of devices on your TV/receiver
  osd_name: "HDMI Bridge" # Optional. Defaults to "esphome"
  
  # By default, promiscuous mode is disabled, so the component only handles directly-address messages (matching
  # the address configured above) and broadcast messages. Enabling promiscuous mode will make the component
  # listen for all messages (both in logs and the on_message triggers)
  promiscuous_mode: true # Optional. Defaults to false
  
  # By default, monitor mode is disabled, so the component can send messages and acknowledge incoming messages.
  # Enabling monitor mode lets the component act as a passive listener, disabling active manipulation of the CEC bus.
  monitor_mode: false # Optional. Defaults to false

  on_message:

    - then:
        #Send CEC messages via MQTT in CEC-O-Matic format
        mqtt.publish:
          topic: cec_messages
          payload: !lambda |-
            return hdmi_cec::Frame(source, destination, data).to_string(true);
      
      #CEC message decoder (human-readable translation)
    - then:
        - lambda: |-
            hdmi_cec::Frame frame = hdmi_cec::Frame(source, destination, data);
            id(cec_raw_message).publish_state(frame.to_string(true));
            id(cec_translated_message).publish_state(frame.to_string());

text_sensor: #Consider excluding these sensors from you Home Assistant database to save space.
  - platform: template
    name: "HDMI CEC Raw Message"
    id: cec_raw_message #Do not delete if used with CEC message decoder
    update_interval: never

  - platform: template
    name: "HDMI CEC Translated Message"
    id: cec_translated_message #Do not delete if used with CEC message decoder
    update_interval: never


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

---

## ✅ Compatibility

| Platform  | Supported | Notes                             |
| --------- | --------- | --------------------------------- |
| ESP32     | ✅         | Fully supported (use type: esp-idf for ESP32-C3 |
| ESP8266   | ✅         | Tested and works                  |
| RP2040    | ✅         | Tested and works                  |
| LibreTiny | ❌         | Not supported                     |

---

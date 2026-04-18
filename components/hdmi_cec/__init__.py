import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.components import text_sensor
from esphome.components import binary_sensor
from esphome.components import button as button_platform
from esphome.components import switch as switch_platform
from esphome.components import number as number_platform
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_TRIGGER_ID,
    CONF_DEVICE_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID, CORE, Define
from esphome.core.config import Device
from esphome.helpers import fnv1a_32bit_hash

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@Palakis"]
AUTO_LOAD = ["text_sensor", "binary_sensor", "button", "switch", "number"]

CONF_PIN = "pin"
CONF_ADDRESS = "address"
CONF_PHYSICAL_ADDRESS = "physical_address"
CONF_PROMISCUOUS_MODE = "promiscuous_mode"
CONF_MONITOR_MODE = "monitor_mode"
CONF_DECODE_MESSAGES = "decode_messages"
CONF_OSD_NAME = "osd_name"
CONF_ON_MESSAGE = "on_message"
CONF_DEVICE_TYPE = "device_type"

CONF_LOGICAL_ADDRESS = "logical_address"
CONF_ACTIVE_SOURCE = "active_source"
CONF_SCAN_BUS_BUTTON = "scan_bus"
CONF_SOURCE = "source"
CONF_DESTINATION = "destination"
CONF_OPCODE = "opcode"
CONF_DATA = "data"
CONF_PARENT = "parent"
CONF_OSD_NAME_TARGET = "osd_name"
CONF_VENDOR_ID = "vendor_id"
CONF_SCAN_ON_BOOT = "scan_on_boot"
CONF_SCAN_BOOT_DELAY = "scan_boot_delay"
CONF_ON_SCAN_COMPLETE = "on_scan_complete"

CONF_POWER_POLL_INTERVAL = "power_poll_interval"
CONF_DEVICES = "devices"
CONF_MATCH = "match"
CONF_POWER_SWITCH = "power_switch"
CONF_DEVICE = "device"

DEVICE_TYPES = {
    "tv": 0x00,
    "recording_device": 0x01,
    "tuner": 0x03,
    "playback_device": 0x04,
    "audio_system": 0x05,
    "other": 0xFF,
}

CONTROL_GROUPS = {
    "navigation_buttons": [
        ("Select", 0x00, "mdi:circle-outline"),
        ("Up", 0x01, "mdi:chevron-up"),
        ("Down", 0x02, "mdi:chevron-down"),
        ("Left", 0x03, "mdi:chevron-left"),
        ("Right", 0x04, "mdi:chevron-right"),
        ("Root Menu", 0x09, "mdi:menu"),
        ("Back", 0x0D, "mdi:keyboard-backspace"),
    ],
    "transport_buttons": [
        ("Play", 0x44, "mdi:play"),
        ("Stop", 0x45, "mdi:stop"),
        ("Pause", 0x46, "mdi:pause"),
        ("Record", 0x47, "mdi:record"),
        ("Rewind", 0x48, "mdi:rewind"),
        ("Fast Forward", 0x49, "mdi:fast-forward"),
        ("Play/Pause", 0x61, "mdi:play-pause"),
    ],
    "volume_buttons": [
        ("Volume Up", 0x41, "mdi:volume-plus"),
        ("Volume Down", 0x42, "mdi:volume-minus"),
        ("Mute", 0x43, "mdi:volume-mute"),
    ],
    "power_buttons": [
        ("Power Toggle", 0x40, "mdi:power"),
        ("Power Off", 0x6C, "mdi:power-off"),
        ("Power On", 0x6D, "mdi:power-on"),
    ],
    "number_buttons": [
        ("0", 0x20, "mdi:numeric-0"),
        ("1", 0x21, "mdi:numeric-1"),
        ("2", 0x22, "mdi:numeric-2"),
        ("3", 0x23, "mdi:numeric-3"),
        ("4", 0x24, "mdi:numeric-4"),
        ("5", 0x25, "mdi:numeric-5"),
        ("6", 0x26, "mdi:numeric-6"),
        ("7", 0x27, "mdi:numeric-7"),
        ("8", 0x28, "mdi:numeric-8"),
        ("9", 0x29, "mdi:numeric-9"),
        ("Dot", 0x2A, "mdi:circle-small"),
        ("Enter", 0x2B, "mdi:keyboard-return"),
        ("Clear", 0x2C, "mdi:backspace-outline"),
        ("Number Entry Mode", 0x1D, "mdi:dialpad"),
        ("11", 0x1E, "mdi:numeric"),
        ("12", 0x1F, "mdi:numeric"),
    ],
    "channel_buttons": [
        ("Channel Up", 0x30, "mdi:arrow-up-bold"),
        ("Channel Down", 0x31, "mdi:arrow-down-bold"),
        ("Previous Channel", 0x32, "mdi:arrow-u-left-top"),
    ],
    "color_buttons": [
        ("Blue", 0x71, "mdi:square-rounded"),
        ("Red", 0x72, "mdi:square-rounded"),
        ("Green", 0x73, "mdi:square-rounded"),
        ("Yellow", 0x74, "mdi:square-rounded"),
    ],
}

# Sensor definitions: key -> (platform_type, setter_name, default_icon, entity_category)
# "text" = text_sensor, "binary" = binary_sensor
SENSOR_DEFS = {
    "osd_name_sensor": (
        "text",
        "set_osd_name_sensor",
        "mdi:label",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "device_type_sensor": (
        "text",
        "set_device_type_sensor",
        "mdi:devices",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "vendor_id_sensor": (
        "text",
        "set_vendor_id_sensor",
        "mdi:factory",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "vendor_name_sensor": (
        "text",
        "set_vendor_name_sensor",
        "mdi:factory",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "physical_address_sensor": (
        "text",
        "set_physical_address_sensor",
        "mdi:hdmi-port",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "power_status_sensor": ("text", "set_power_status_sensor", "mdi:power", None),
    "cec_version_sensor": (
        "text",
        "set_cec_version_sensor",
        "mdi:information-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "active_source_sensor": (
        "binary",
        "set_active_source_sensor",
        "mdi:video-input-hdmi",
        None,
    ),
    "last_seen_sensor": (
        "text",
        "set_last_seen_sensor",
        "mdi:clock-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

# Human-readable labels for auto-naming entities
SENSOR_LABELS = {
    "osd_name_sensor": "OSD Name",
    "device_type_sensor": "Device Type",
    "vendor_id_sensor": "Vendor ID",
    "vendor_name_sensor": "Vendor Name",
    "physical_address_sensor": "Physical Address",
    "power_status_sensor": "Power Status",
    "cec_version_sensor": "CEC Version",
    "active_source_sensor": "Active Source",
    "last_seen_sensor": "Last Seen",
}


def validate_data_array(value):
    if isinstance(value, list):
        return cv.Schema([cv.hex_uint8_t])(value)
    raise cv.Invalid("data must be a list of bytes")


def validate_osd_name(value):
    if not isinstance(value, str):
        raise cv.Invalid("Must be a string")
    if len(value) < 1:
        raise cv.Invalid("Must be a non-empty string")
    if len(value) > 14:
        raise cv.Invalid("Must not be more than 14-characters long")

    for char in value:
        if not 0x20 <= ord(char) < 0x7E:
            raise cv.Invalid(
                f"character '{char}' ({ord(char)}) is outside of the supported character range (0x20..0x7e)"
            )

    return value


hdmi_cec_ns = cg.esphome_ns.namespace("hdmi_cec")
HDMICEC = hdmi_cec_ns.class_("HDMICEC", cg.Component)
CECRemoteDevice = hdmi_cec_ns.class_("CECRemoteDevice")
CECControlButton = hdmi_cec_ns.class_("CECControlButton", button_platform.Button)
CECPowerSwitch = hdmi_cec_ns.class_("CECPowerSwitch", switch_platform.Switch)
MessageTrigger = hdmi_cec_ns.class_(
    "MessageTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.std_vector.template(cg.uint8)),
)
SendAction = hdmi_cec_ns.class_("SendAction", automation.Action)
SendToOsdNameAction = hdmi_cec_ns.class_("SendToOsdNameAction", automation.Action)
SendToPhysicalAddressAction = hdmi_cec_ns.class_("SendToPhysicalAddressAction", automation.Action)
SendToVendorAndTypeAction = hdmi_cec_ns.class_("SendToVendorAndTypeAction", automation.Action)
SendToDeviceAction = hdmi_cec_ns.class_("SendToDeviceAction", automation.Action)
ScanBusAction = hdmi_cec_ns.class_("ScanBusAction", automation.Action)
ScanCompleteTrigger = hdmi_cec_ns.class_("ScanCompleteTrigger", automation.Trigger.template())
ScanButton = hdmi_cec_ns.class_("ScanButton", button_platform.Button)
CECPowerPollNumber = hdmi_cec_ns.class_("CECPowerPollNumber", number_platform.Number, cg.Component)


TextSensor = text_sensor.TextSensor
BinarySensor = binary_sensor.BinarySensor


def _build_individual_sensor_schema(sensor_key):
    platform_type, setter, icon, entity_cat = SENSOR_DEFS[sensor_key]
    kwargs = {}
    if icon:
        kwargs["icon"] = icon
    if entity_cat:
        kwargs["entity_category"] = entity_cat
    if platform_type == "text":
        return text_sensor.text_sensor_schema(**kwargs)
    else:
        return binary_sensor.binary_sensor_schema(**kwargs)


_BARE_ENTITY = {"__bare__": True}


def _optional_entity(schema):
    """Accept a full schema dict, or None (bare key like 'power:') to use all defaults."""

    def validator(value):
        if value is None:
            return _BARE_ENTITY
        return schema(value)

    return validator


def _is_bare(conf):
    return isinstance(conf, dict) and conf.get("__bare__") is True


def validate_match_criteria(config):
    match = config.get(CONF_MATCH)
    if match is None or len(match) == 0:
        raise cv.Invalid("At least one match criterion is required inside 'match:'")
    return config


MATCH_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_OSD_NAME): cv.string,
        cv.Optional(CONF_VENDOR_ID): cv.uint32_t,
        cv.Optional(CONF_DEVICE_TYPE): cv.enum(DEVICE_TYPES, lower=True),
        cv.Optional(CONF_PHYSICAL_ADDRESS): cv.uint16_t,
    }
)

CEC_DEVICE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CECRemoteDevice),
            cv.Required(CONF_NAME): cv.string,
            cv.Required(CONF_MATCH): MATCH_SCHEMA,
            # Power switch (accepts bare 'power:' or 'power: {name: ...}')
            cv.Optional(CONF_POWER_SWITCH): _optional_entity(
                switch_platform.switch_schema(
                    CECPowerSwitch,
                    icon="mdi:power",
                )
            ),
            # Sensor entities (each accepts bare key or full schema)
            **{cv.Optional(key): _optional_entity(_build_individual_sensor_schema(key)) for key in SENSOR_DEFS},
            # Control button groups (bare key or true enables the group)
            **{cv.Optional(group_key): lambda v: True if v is None else cv.boolean(v) for group_key in CONTROL_GROUPS},
        }
    ),
    validate_match_criteria,
)


def validate_address_config(config):
    has_device_type = CONF_DEVICE_TYPE in config or CONF_ADDRESS not in config
    if has_device_type and config.get(CONF_MONITOR_MODE, False):
        raise cv.Invalid("'device_type' cannot be used with 'monitor_mode' (passive devices don't negotiate)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(HDMICEC),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_ADDRESS): cv.int_range(min=0, max=15),
            cv.Required(CONF_PHYSICAL_ADDRESS): cv.uint16_t,
            cv.Optional(CONF_DEVICE_TYPE): cv.enum(DEVICE_TYPES, lower=True),
            cv.Optional(CONF_PROMISCUOUS_MODE, False): cv.boolean,
            cv.Optional(CONF_MONITOR_MODE, False): cv.boolean,
            cv.Optional(CONF_DECODE_MESSAGES, True): cv.boolean,
            cv.Optional(CONF_OSD_NAME, "esphome"): validate_osd_name,
            cv.Optional(CONF_LOGICAL_ADDRESS, default={"name": "Logical Address"}): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:mail",
            ),
            cv.Optional(CONF_ACTIVE_SOURCE, default={"name": "Active Source"}): text_sensor.text_sensor_schema(
                icon="mdi:video-input-hdmi",
            ),
            cv.Optional(CONF_SCAN_BUS_BUTTON, default={"name": "Scan CEC Bus"}): button_platform.button_schema(
                ScanButton,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:magnify",
            ),
            cv.Optional(CONF_SCAN_ON_BOOT, default=True): cv.boolean,
            cv.Optional(CONF_SCAN_BOOT_DELAY, default="3s"): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_POWER_POLL_INTERVAL, default={"name": "Power Poll Interval"}
            ): number_platform.number_schema(
                CECPowerPollNumber,
                icon="mdi:timer-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                unit_of_measurement="ms",
            ),
            cv.Optional(CONF_ON_SCAN_COMPLETE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ScanCompleteTrigger),
                }
            ),
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MessageTrigger),
                    cv.Optional(CONF_SOURCE): cv.int_range(min=0, max=15),
                    cv.Optional(CONF_DESTINATION): cv.int_range(min=0, max=15),
                    cv.Optional(CONF_OPCODE): cv.uint8_t,
                    cv.Optional(CONF_DATA): validate_data_array,
                }
            ),
            cv.Optional(CONF_DEVICES): cv.ensure_list(CEC_DEVICE_SCHEMA),
        }
    ),
    validate_address_config,
)


def _update_device_count_define(count_to_add):
    """Update ESPHOME_DEVICE_COUNT define, adding to any existing value."""
    existing_count = 0
    to_remove = None
    for d in CORE.defines:
        if isinstance(d, Define) and d.name == "ESPHOME_DEVICE_COUNT":
            existing_count = d.value
            to_remove = d
            break
    if to_remove is not None:
        CORE.defines.discard(to_remove)
    cg.add_define("USE_DEVICES")
    cg.add_define("ESPHOME_DEVICE_COUNT", existing_count + count_to_add)


async def _create_sensor_entity(sensor_key, sensor_conf, device_id_str, remote_dev_var):
    """Create a text_sensor or binary_sensor entity and bind it to the remote device."""
    platform_type, setter, icon, entity_cat = SENSOR_DEFS[sensor_key]

    if platform_type == "text":
        sens = await text_sensor.new_text_sensor(sensor_conf)
    else:
        sens = await binary_sensor.new_binary_sensor(sensor_conf)

    cg.add(getattr(remote_dev_var, setter)(sens))
    return sens


async def to_code(config):
    if config[CONF_DECODE_MESSAGES] == True:
        cg.add_define("USE_CEC_DECODER")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_LOGICAL_ADDRESS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LOGICAL_ADDRESS])
        cg.add(var.set_address_sensor(sens))

    if CONF_ACTIVE_SOURCE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_ACTIVE_SOURCE])
        cg.add(var.set_active_source_sensor(sens))

    if CONF_SCAN_BUS_BUTTON in config:
        btn = await button_platform.new_button(config[CONF_SCAN_BUS_BUTTON])
        cg.add(btn.set_parent(var))

    cec_pin_ = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(cec_pin_))

    if CONF_ADDRESS in config:
        _LOGGER.warning(
            "hdmi_cec: Manually setting 'address' bypasses logical address auto-negotiation and is deprecated. This can cause conflicts with other CEC devices on the bus. Remove 'address' to allow auto-negotiation."
        )
        cg.add(var.set_address(config[CONF_ADDRESS]))
    else:
        cg.add(var.set_address(0x0F))  # Unregistered until negotiation

    if CONF_ADDRESS not in config:
        cg.add(var.set_device_type(config.get(CONF_DEVICE_TYPE, DEVICE_TYPES["other"])))

    cg.add(var.set_physical_address(config[CONF_PHYSICAL_ADDRESS]))
    cg.add(var.set_promiscuous_mode(config[CONF_PROMISCUOUS_MODE]))
    cg.add(var.set_monitor_mode(config[CONF_MONITOR_MODE]))

    osd_name_bytes = bytes(config[CONF_OSD_NAME], "ascii", "ignore")  # convert string to ascii bytes
    osd_name_bytes = [x for x in osd_name_bytes]  # convert byte array to int array
    osd_name_bytes = cg.std_vector.template(cg.uint8)(osd_name_bytes)
    cg.add(var.set_osd_name_bytes(osd_name_bytes))

    cg.add(var.set_scan_on_boot(config[CONF_SCAN_ON_BOOT]))
    cg.add(var.set_scan_boot_delay(config[CONF_SCAN_BOOT_DELAY]))

    if CONF_POWER_POLL_INTERVAL in config:
        num = await number_platform.new_number(
            config[CONF_POWER_POLL_INTERVAL],
            min_value=0,
            max_value=60000,
            step=100,
        )
        await cg.register_component(num, {})
        cg.add(num.set_parent(var))
        cg.add(var.set_power_poll_number(num))

    for conf in config.get(CONF_ON_SCAN_COMPLETE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_MESSAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)

        source = conf.get(CONF_SOURCE)
        if source is not None:
            cg.add(trigger.set_source(source))

        destination = conf.get(CONF_DESTINATION)
        if destination is not None:
            cg.add(trigger.set_destination(destination))

        opcode = conf.get(CONF_OPCODE)
        if opcode is not None:
            cg.add(trigger.set_opcode(opcode))

        data = conf.get(CONF_DATA)
        if data is not None:
            cg.add(trigger.set_data(data))

        await automation.build_automation(
            trigger,
            [
                (cg.uint8, "source"),
                (cg.uint8, "destination"),
                (cg.std_vector.template(cg.uint8), "data"),
            ],
            conf,
        )

    # User-defined devices
    device_configs = config.get(CONF_DEVICES, [])
    if device_configs:
        _update_device_count_define(len(device_configs))

    for dev_conf in device_configs:
        dev_id = dev_conf[CONF_ID]
        dev_name = dev_conf[CONF_NAME]
        dev_id_str = str(dev_id.id)

        # Create CECRemoteDevice
        dev_var = cg.new_Pvariable(dev_id)
        cg.add(dev_var.set_parent(var))
        cg.add(dev_var.set_name(dev_name))
        cg.add(dev_var.set_yaml_id(dev_id_str))
        cg.add(var.add_remote_device(dev_var))

        # Create ESPHome sub-device for HA grouping
        sub_device_var_id = ID(f"{dev_id_str}_device", is_declaration=True, type=Device)
        sub_device_id = cg.new_Pvariable(sub_device_var_id)
        cg.add(sub_device_id.set_device_id(fnv1a_32bit_hash(dev_id_str)))
        cg.add(sub_device_id.set_name(dev_name))
        cg.add(cg.App.register_device(sub_device_id))
        device_ref = ID(f"{dev_id_str}_device", type=Device)

        # Set match criteria
        match = dev_conf[CONF_MATCH]
        if CONF_OSD_NAME in match:
            cg.add(dev_var.set_match_osd_name(match[CONF_OSD_NAME]))
        if CONF_VENDOR_ID in match:
            cg.add(dev_var.set_match_vendor_id(match[CONF_VENDOR_ID]))
        if CONF_DEVICE_TYPE in match:
            cg.add(dev_var.set_match_device_type(match[CONF_DEVICE_TYPE]))
        if CONF_PHYSICAL_ADDRESS in match:
            cg.add(dev_var.set_match_physical_address(match[CONF_PHYSICAL_ADDRESS]))

        # Helper: ensure entity name is prefixed with device name for HA display
        def _prefixed_name(name, default_label):
            """Use the user-provided name if set, otherwise auto-generate '{dev_name} {label}'."""
            if name:
                return name
            return f"{dev_name} {default_label}"

        # Collect all sensors to create (individual keys + shorthand list)
        sensors_to_create = {}

        # Individual sensor entities (full schema — may have user name or be bare)
        for sensor_key in SENSOR_DEFS:
            if sensor_key in dev_conf:
                raw_conf = dev_conf[sensor_key]
                label = SENSOR_LABELS[sensor_key]
                sensor_id_str = f"{dev_id_str}_{sensor_key}"
                platform_type = SENSOR_DEFS[sensor_key][0]
                if _is_bare(raw_conf):
                    schema = _build_individual_sensor_schema(sensor_key)
                    sensor_conf = schema({CONF_NAME: f"{dev_name} {label}"})
                else:
                    sensor_conf = dict(raw_conf)
                    sensor_conf[CONF_NAME] = _prefixed_name(sensor_conf.get(CONF_NAME), label)
                # Ensure unique ID
                if platform_type == "text":
                    sensor_conf[CONF_ID] = ID(sensor_id_str, is_declaration=True, type=text_sensor.TextSensor)
                else:
                    sensor_conf[CONF_ID] = ID(
                        sensor_id_str,
                        is_declaration=True,
                        type=binary_sensor.BinarySensor,
                    )
                sensor_conf[CONF_DEVICE_ID] = device_ref
                sensors_to_create[sensor_key] = sensor_conf

        # Create all sensor entities
        for sensor_key, sensor_conf in sensors_to_create.items():
            await _create_sensor_entity(sensor_key, sensor_conf, dev_id_str, dev_var)

        # Power switch
        if CONF_POWER_SWITCH in dev_conf:
            raw_power = dev_conf[CONF_POWER_SWITCH]
            if _is_bare(raw_power):
                power_schema = switch_platform.switch_schema(CECPowerSwitch, icon="mdi:power")
                power_conf = power_schema({CONF_NAME: f"{dev_name} Power"})
            else:
                power_conf = dict(raw_power)
                power_conf[CONF_NAME] = _prefixed_name(power_conf.get(CONF_NAME), "Power")
            power_conf[CONF_ID] = ID(f"{dev_id_str}_power", is_declaration=True, type=CECPowerSwitch)
            power_conf[CONF_DEVICE_ID] = device_ref
            sw = await switch_platform.new_switch(power_conf)
            cg.add(sw.set_parent(var))
            cg.add(sw.set_remote_device(dev_var))
            cg.add(dev_var.set_power_switch(sw))

        # Control button groups
        for group_name, buttons in CONTROL_GROUPS.items():
            if not dev_conf.get(group_name):
                continue
            for btn_name, ui_code, btn_icon in buttons:
                safe_name = btn_name.lower().replace("/", "_").replace(" ", "_")
                btn_id_str = f"{dev_id_str}_{safe_name}"
                btn_schema = button_platform.button_schema(CECControlButton, icon=btn_icon)
                btn_conf = btn_schema({CONF_NAME: f"{dev_name} {btn_name}"})
                btn_conf[CONF_ID] = ID(btn_id_str, is_declaration=True, type=CECControlButton)
                btn_conf[CONF_DEVICE_ID] = device_ref
                btn = await button_platform.new_button(btn_conf)
                cg.add(btn.set_parent(var))
                cg.add(btn.set_remote_device(dev_var))
                cg.add(btn.set_ui_command(ui_code))


@automation.register_action(
    "hdmi_cec.send",
    SendAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
        cv.Optional(CONF_SOURCE): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_DESTINATION): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_DATA): cv.templatable(validate_data_array),
    },
)
async def send_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    var = cg.new_Pvariable(action_id, template_args, parent)

    source_ = config.get(CONF_SOURCE)
    if source_ is not None:
        source_template_ = await cg.templatable(source_, args, cg.uint8)
        cg.add(var.set_source(source_template_))

    destination_template_ = await cg.templatable(config.get(CONF_DESTINATION), args, cg.uint8)
    cg.add(var.set_destination(destination_template_))

    data_vec_ = cg.std_vector.template(cg.uint8)
    data_template_ = await cg.templatable(config.get(CONF_DATA), args, data_vec_, data_vec_)
    cg.add(var.set_data(data_template_))

    return var


@automation.register_action(
    "hdmi_cec.scan_bus",
    ScanBusAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
    },
)
async def scan_bus_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    return cg.new_Pvariable(action_id, template_args, parent)


@automation.register_action(
    "hdmi_cec.send_to_osd_name",
    SendToOsdNameAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
        cv.Optional(CONF_SOURCE): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_OSD_NAME_TARGET): cv.templatable(cv.string),
        cv.Required(CONF_DATA): cv.templatable(validate_data_array),
    },
)
async def send_to_osd_name_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    var = cg.new_Pvariable(action_id, template_args, parent)

    source_template_ = await cg.templatable(config.get(CONF_SOURCE), args, cg.uint8)
    if source_template_ is not None:
        cg.add(var.set_source(source_template_))

    osd_name_template_ = await cg.templatable(config[CONF_OSD_NAME_TARGET], args, cg.std_string)
    cg.add(var.set_osd_name(osd_name_template_))

    data_vec_ = cg.std_vector.template(cg.uint8)
    data_template_ = await cg.templatable(config[CONF_DATA], args, data_vec_, data_vec_)
    cg.add(var.set_data(data_template_))

    return var


@automation.register_action(
    "hdmi_cec.send_to_physical_address",
    SendToPhysicalAddressAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
        cv.Optional(CONF_SOURCE): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_PHYSICAL_ADDRESS): cv.templatable(cv.uint16_t),
        cv.Required(CONF_DATA): cv.templatable(validate_data_array),
    },
)
async def send_to_physical_address_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    var = cg.new_Pvariable(action_id, template_args, parent)

    source_template_ = await cg.templatable(config.get(CONF_SOURCE), args, cg.uint8)
    if source_template_ is not None:
        cg.add(var.set_source(source_template_))

    phys_template_ = await cg.templatable(config[CONF_PHYSICAL_ADDRESS], args, cg.uint16)
    cg.add(var.set_physical_address(phys_template_))

    data_vec_ = cg.std_vector.template(cg.uint8)
    data_template_ = await cg.templatable(config[CONF_DATA], args, data_vec_, data_vec_)
    cg.add(var.set_data(data_template_))

    return var


@automation.register_action(
    "hdmi_cec.send_to_vendor_and_type",
    SendToVendorAndTypeAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
        cv.Optional(CONF_SOURCE): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_VENDOR_ID): cv.templatable(cv.uint32_t),
        cv.Required(CONF_DEVICE_TYPE): cv.templatable(cv.uint8_t),
        cv.Required(CONF_DATA): cv.templatable(validate_data_array),
    },
)
async def send_to_vendor_and_type_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    var = cg.new_Pvariable(action_id, template_args, parent)

    source_template_ = await cg.templatable(config.get(CONF_SOURCE), args, cg.uint8)
    if source_template_ is not None:
        cg.add(var.set_source(source_template_))

    vendor_template_ = await cg.templatable(config[CONF_VENDOR_ID], args, cg.uint32)
    cg.add(var.set_vendor_id(vendor_template_))

    dtype_template_ = await cg.templatable(config[CONF_DEVICE_TYPE], args, cg.uint8)
    cg.add(var.set_device_type(dtype_template_))

    data_vec_ = cg.std_vector.template(cg.uint8)
    data_template_ = await cg.templatable(config[CONF_DATA], args, data_vec_, data_vec_)
    cg.add(var.set_data(data_template_))

    return var


@automation.register_action(
    "hdmi_cec.send_to_device",
    SendToDeviceAction,
    {
        cv.GenerateID(CONF_PARENT): cv.use_id(HDMICEC),
        cv.Optional(CONF_SOURCE): cv.templatable(cv.int_range(min=0, max=15)),
        cv.Required(CONF_DEVICE): cv.use_id(CECRemoteDevice),
        cv.Required(CONF_DATA): cv.templatable(validate_data_array),
    },
)
async def send_to_device_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_PARENT])
    var = cg.new_Pvariable(action_id, template_args, parent)

    source_template_ = await cg.templatable(config.get(CONF_SOURCE), args, cg.uint8)
    if source_template_ is not None:
        cg.add(var.set_source(source_template_))

    device = await cg.get_variable(config[CONF_DEVICE])
    cg.add(var.set_device(device))

    data_vec_ = cg.std_vector.template(cg.uint8)
    data_template_ = await cg.templatable(config[CONF_DATA], args, data_vec_, data_vec_)
    cg.add(var.set_data(data_template_))

    return var

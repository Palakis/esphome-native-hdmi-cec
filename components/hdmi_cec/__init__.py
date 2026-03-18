import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.components import text_sensor
from esphome.components import button as button_platform
from esphome.const import CONF_ID, CONF_TRIGGER_ID, ENTITY_CATEGORY_DIAGNOSTIC

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@Palakis"]
AUTO_LOAD = ["text_sensor", "button"]

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

DEVICE_TYPES = {
    "tv": 0x00,
    "recording_device": 0x01,
    "tuner": 0x03,
    "playback_device": 0x04,
    "audio_system": 0x05,
    "other": 0xFF,
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
MessageTrigger = hdmi_cec_ns.class_(
    "MessageTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.std_vector.template(cg.uint8)),
)
SendAction = hdmi_cec_ns.class_("SendAction", automation.Action)
SendToOsdNameAction = hdmi_cec_ns.class_("SendToOsdNameAction", automation.Action)
SendToPhysicalAddressAction = hdmi_cec_ns.class_("SendToPhysicalAddressAction", automation.Action)
SendToVendorAndTypeAction = hdmi_cec_ns.class_("SendToVendorAndTypeAction", automation.Action)
ScanBusAction = hdmi_cec_ns.class_("ScanBusAction", automation.Action)
ScanCompleteTrigger = hdmi_cec_ns.class_("ScanCompleteTrigger", automation.Trigger.template())
ScanButton = hdmi_cec_ns.class_("ScanButton", button_platform.Button)


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
        }
    ),
    validate_address_config,
)


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

    source_template_ = await cg.templatable(config.get(CONF_SOURCE), args, cg.uint8)
    if source_template_ is not None:
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

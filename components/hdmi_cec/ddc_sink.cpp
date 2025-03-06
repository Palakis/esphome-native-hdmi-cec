#include "ddc_sink.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {
namespace ddc {

static const char *const TAG = "hdmi_cec::ddc";
static const uint8_t DDC_EDID_ADDRESS = 0x50;
static const uint8_t DDC_SEGMENT_POINTER_ADDRESS = 0x00;

optional<uint16_t> Sink::read_physical_address() {
  // reset segment pointer
  set_segment_pointer_(0x00);

  set_i2c_address(DDC_EDID_ADDRESS);

  // read base EDID block
  uint8_t edid[128];
  read_register(0x00, edid, sizeof(edid));

  // check if the header of the base EDID block is valid
  if (validate_edid_header_(edid, sizeof(edid)) == false) {
    ESP_LOGW(TAG, "Invalid EDID header");
  }

  // check if the base EDID block is valid 
  if (validate_edid_block_(edid, sizeof(edid)) == false) {
    ESP_LOGW(TAG, "EDID base block checksum error. Potentially invalid EDID");
  }

  uint8_t edid_version = edid[0x12];
  uint8_t edid_revision = edid[0x13];
  ESP_LOGD(TAG, "EDID version: %d.%d", edid_version, edid_revision);

  // check if EDID has an extension block
  if (edid[0x7E] == 0x00) {
    ESP_LOGW(TAG, "Cannot read physical address from DDC: no EDID extension blocks detected");
    return optional<uint16_t>();
  }

  // read the first extension block
  uint8_t ext_block[128];
  read_register(0x80, ext_block, sizeof(ext_block));

  // check if the extension block is a valid CEA-861 block
  if (ext_block[0] != 0x02) {
    ESP_LOGW(TAG, "Cannot read physical address from DDC: invalid first EDID extension block (not CEA-861 compliant)");
    return optional<uint16_t>();
  }

  return optional<uint16_t>();
}

void Sink::set_segment_pointer_(uint8_t segment_pointer, bool stop) {
  uint8_t current_address = address_;

  set_i2c_address(DDC_SEGMENT_POINTER_ADDRESS);
  const uint8_t data[] = { segment_pointer };
  write(data, sizeof(data), stop);

  set_i2c_address(current_address);
}

bool Sink::validate_edid_header_(const uint8_t *data, size_t max_len) {
  if (max_len < 8) {
    return false;
  }

  // check for this pattern: 00 FF FF FF FF FF FF 00
  for (size_t i = 0; i < 7; i++) {
    if ((i == 0 || i == 7) && data[i] != 0x00) {
      return false;
    }

    if (data[i] != 0xFF) {
      return false;
    }
  }

  return true;
}

bool Sink::validate_edid_block_(const uint8_t *data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += data[i];
  }
  return (sum % 256) == 0;
}

}
}
}

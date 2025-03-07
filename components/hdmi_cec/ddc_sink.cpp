#include "ddc_sink.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {
namespace ddc {

static const char *const TAG = "hdmi_cec::ddc";
static const uint8_t DDC_EDID_ADDRESS = 0x50;
static const uint8_t DDC_SEGMENT_POINTER_ADDRESS = 0x60;

optional<uint16_t> Sink::read_physical_address() {
  // reset segment pointer
  set_segment_pointer_(0x00);

  set_i2c_address(DDC_EDID_ADDRESS);

  {
    // read base EDID block
    std::vector<uint8_t> edid(128);
    read_register(0x00, edid.data(), edid.size());

    // check if the header of the base EDID block is valid
    if (validate_edid_header_(edid) == false) {
      ESP_LOGW(TAG, "Invalid EDID header");
    }

    // validate the base EDID block
    if (validate_edid_block_(edid) == false) {
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
  }

  // read the first extension block
  std::vector<uint8_t> ext_block(128);
  read_register(0x80, ext_block.data(), ext_block.size());

  // check if the extension block is a valid CEA-861 block
  if (ext_block[0] != 0x02) {
    ESP_LOGW(TAG, "Cannot read physical address from DDC: invalid first EDID extension block (not CEA-861 compliant)");
    return optional<uint16_t>();
  }

  // validate the CEA-861-D block
  if (validate_edid_block_(ext_block) == false) {
    ESP_LOGW(TAG, "CEA-861-D checksum error. Potentially invalid EDID");
  }

  // check the CEA-861-D revision. The reserved data block was introduced in Revision 3.
  static const uint8_t expected_cea_revision = 0x03;
  if (ext_block[1] < expected_cea_revision) {
    ESP_LOGW(TAG, "Cannot read physical address from DDC: invalid CEA-861 revision (got %d, expected %d)", ext_block[1], expected_cea_revision);
    return optional<uint16_t>();
  }

  // extract the reserved data block 
  uint8_t timing_descriptor_offset = ext_block[2];
  std::vector<uint8_t> cea_data_block(ext_block.begin() + 4, ext_block.begin() + timing_descriptor_offset - 1);

  // read through all the blocks of the reserved data block to find the Vendor-Specific Data Block
  size_t i = 0;
  while (i < cea_data_block.size()) {
    uint8_t header = cea_data_block[i];
    uint8_t tag = (header & 0xE0) >> 5;
    uint8_t length = header & 0x1F;

    if (tag == 0x03) {
      // found the Vendor-Specific Data Block
      if (length < 5) {
        ESP_LOGW(TAG, "Cannot read physical address from DDC: Vendor-Specific Data Block too short");
        return optional<uint16_t>();
      }

      if (i + length >= cea_data_block.size()) {
        ESP_LOGW(TAG, "Cannot read physical address from DDC: out-of-bounds block length");
        return optional<uint16_t>();
      }

      uint32_t ieee_id = cea_data_block[i + 1] | (cea_data_block[i + 2] << 8) | (cea_data_block[i + 3] << 16);
      if (ieee_id == 0x000C03) {
        // it's a HDMI Vendor-Specific Data Block
        uint16_t physical_address = cea_data_block[i + 4] << 8 | cea_data_block[i + 5];
        return optional<uint16_t>(physical_address);
      }
    }

    i += length;
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

bool Sink::validate_edid_header_(std::vector<uint8_t> &data) {
  if (data.size() < 8) {
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

bool Sink::validate_edid_block_(std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (const uint8_t &byte : data) {
    sum += byte;
  }
  return (sum % 256) == 0;
}

}
}
}

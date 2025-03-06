#pragma once

#include <vector>
#include <queue>
#include <optional>

#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace hdmi_cec {
namespace ddc {

using namespace i2c;

class Sink : I2CDevice {
public:
  Sink(I2CBus *i2c_bus) : I2CDevice() {
    set_i2c_bus(i2c_bus);
  }

  optional<uint16_t> read_physical_address();

protected:
  void set_segment_pointer_(uint8_t segment_pointer, bool stop = false);
  bool validate_edid_header_(const uint8_t *data, size_t max_len);
  bool validate_edid_block_(const uint8_t *data, size_t len);
};

}
}
}

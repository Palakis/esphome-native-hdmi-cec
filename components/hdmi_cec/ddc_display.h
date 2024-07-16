#pragma once

#include <vector>
#include <queue>
#include <optional>

#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace hdmi_cec {

using namespace i2c;

class DDCDisplay : I2CDevice {
public:
  DDCDisplay(I2CBus *i2c_bus) : I2CDevice() {
    set_i2c_bus(i2c_bus);
    set_i2c_address(0x0); // TODO put EDID base address
  }

  optional<uint16_t> read_physical_address();
};

}
}

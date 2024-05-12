#pragma once

#include <vector>
#include <queue>
#include <optional>

#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace hdmi_cec {

using namespace i2c;

class DDCReader : I2CDevice {
public:
  DDCReader(I2CBus *i2c_bus) : I2CDevice() {
    set_i2c_bus(i2c_bus);
    set_i2c_address(0x0); // TODO get the actual base address from the DDC spec
  }
protected:
  I2CDevice prout_;
};

}
}

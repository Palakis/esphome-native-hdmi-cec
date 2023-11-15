#include "hdmi_cec.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";
static const uint32_t START_BIT_MIN_US = 3500;
static const uint32_t HIGH_BIT_MIN_US = 400;
static const uint32_t HIGH_BIT_MAX_US = 800;

void HDMICEC::setup() {
  this->isr_pin_ = this->pin_->to_isr();
  this->recv_frame_buffer_.reserve(16); // max 16 bytes per CEC frame
  this->pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  this->pin_->attach_interrupt(HDMICEC::gpio_intr, this, gpio::INTERRUPT_ANY_EDGE);
}

void HDMICEC::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC");
  LOG_PIN("  pin: ", this->pin_);
  ESP_LOGCONFIG(TAG, "  address: %x", this->address_);
  ESP_LOGCONFIG(TAG, "  promiscuous mode: %s", (promiscuous_mode_ ? "yes" : "no"));
}

void HDMICEC::loop() {
  while(!this->recv_queue_.empty()) {
    auto frame = this->recv_queue_.front();
    this->recv_queue_.pop();

    if (frame.size() == 1) {
      // TODO respond to pings to our address
      // for now, ignore pings
      continue;
    }

    uint8_t header = frame[0];
    uint8_t src_addr = ((header & 0xF0) >> 4);
    uint8_t dest_addr = (header & 0x0F);

    if (!promiscuous_mode_ && (dest_addr != 0x0F) && (dest_addr != address_)) {
      // ignore frames not meant for us
      continue;
    }

    ESP_LOGD(TAG, "CEC: %02x -> %02x", src_addr, dest_addr);
    for (int i = 0; i < frame.size(); i++) {
      ESP_LOGD(TAG, "   [%d] = 0x%02X", i, frame[i]);
    }

    uint8_t opcode = frame[1];
    std::vector<uint8_t> data(frame.begin() + 1, frame.end());

    for (auto trigger : message_triggers_) {
      bool can_trigger = (
        (!trigger->source_.has_value()      || (trigger->source_ == src_addr)) &&
        (!trigger->destination_.has_value() || (trigger->destination_ == dest_addr)) &&
        (!trigger->opcode_.has_value()      || (trigger->opcode_ == opcode)) &&
        (!trigger->data_.has_value() ||
          (data.size() == trigger->data_->size() && std::equal(trigger->data_->begin(), trigger->data_->end(), data.begin()))
        )
      );
      if (can_trigger) {
        trigger->trigger(src_addr, dest_addr, data);
      }
    }
  }
}

void HDMICEC::send(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data) {
  // TODO wait for the bus to be free

  // disable the GPIO interrupt
  gpio_interrupt_disabled_ = true;

  send_start_bit();
  
  // for each byte of the frame:
  for (auto it = data.begin(); it != data.end(); ++it) {
    uint8_t current_byte = *it;

    // 1. send the current byte
    for (int8_t i = 7; i >= 0; i--) {
      bool bit_value = ((current_byte >> i) & 0b1);
      send_bit(bit_value);
    }

    // 2. send EOM bit (logic 1 if this is the last byte of the frame)
    bool is_eom = (it == data.end());
    send_bit(is_eom);

    // 3. send ack bit
    send_bit(true); // TODO read ack
  }

  // re-enable the GPIO interrupt
  gpio_interrupt_disabled_ = false;
}

void HDMICEC::send_start_bit() {
  // 1. pull low for 3700 us
  this->pin_->digital_write(false);
  delayMicroseconds(3700);

  // 2. pull high for 800 us
  this->pin_->digital_write(true);
  delayMicroseconds(800);

  // total duration: 4500 us
}

void HDMICEC::send_bit(bool bit_value) {
  // total bit duration:
  // logic 1: pull low for 600 us, then pull high for 1800 us
  // logic 0: pull low for 1500 us, then pull high for 900 us
  static const uint32_t TOTAL_BIT_US = 2400;
  static const uint32_t HIGH_BIT_US = 600;
  static const uint32_t LOW_BIT_US = 1500;

  const uint32_t low_duration_us = (bit_value ? HIGH_BIT_US : LOW_BIT_US);
  const uint32_t high_duration_us = (TOTAL_BIT_US - low_duration_us);

  this->pin_->digital_write(false);
  delayMicroseconds(low_duration_us);
  this->pin_->digital_write(true);
  delayMicroseconds(high_duration_us);
}

void IRAM_ATTR HDMICEC::gpio_intr(HDMICEC *self) {
  if (self->gpio_interrupt_disabled_) {
    return;
  }

  const uint32_t now = micros();
  const bool level = self->isr_pin_.digital_read();

  // on falling edge, store current time as the start of the low pulse
  if (level == false) {
    self->last_falling_edge_us_ = now;
    return;
  }

  // otherwise, it's a rising edge, so it's time to process the pulse length
  if (self->last_falling_edge_us_ == 0) {
    return;
  }

  auto pulse_duration = (micros() - self->last_falling_edge_us_);
  self->last_falling_edge_us_ = 0;

  if (pulse_duration > START_BIT_MIN_US) {
    // start bit detected. reset everything and start receiving
    self->decoder_state_ = DecoderState::ReceivingByte;
    reset_state_variables(self);
    return;
  }

  bool value = (pulse_duration >= HIGH_BIT_MIN_US && pulse_duration <= HIGH_BIT_MAX_US);

  switch (self->decoder_state_) {
    case DecoderState::ReceivingByte: {
      // write bit to the current byte
      self->recv_byte_buffer_ = (self->recv_byte_buffer_ << 1) | (value & 0b1);

      self->recv_bit_counter_++;
      if (self->recv_bit_counter_ >= 8) {
        // if we reached eight bits, push the current byte to the frame buffer
        self->recv_frame_buffer_.push_back(self->recv_byte_buffer_);

        self->recv_bit_counter_ = 0;
        self->recv_byte_buffer_ = 0;

        self->decoder_state_ = DecoderState::WaitingForEOM;
      } else {
        self->decoder_state_ = DecoderState::ReceivingByte;
      }
      break;
    }

    case DecoderState::WaitingForEOM: {
      bool isEOM = (value == 1);
      if (isEOM) {
        // pass frame to app
        self->recv_queue_.push(self->recv_frame_buffer_);
        reset_state_variables(self);
      }

      self->decoder_state_ = (
        isEOM
        ? DecoderState::WaitingForEOMAck
        : DecoderState::WaitingForAck
      );
      break;
    }

    case DecoderState::WaitingForAck: {
      self->decoder_state_ = DecoderState::ReceivingByte;
      break;
    }

    case DecoderState::WaitingForEOMAck: {
      self->decoder_state_ = DecoderState::Idle;
      break;
    }

    default: {
      self->decoder_state_ = DecoderState::ReceivingByte;
      reset_state_variables(self);
      break;
    }
  }
}

void IRAM_ATTR HDMICEC::reset_state_variables(HDMICEC *self) {
  self->recv_bit_counter_ = 0;
  self->recv_byte_buffer_ = 0x0;
  self->recv_frame_buffer_.clear();
  self->recv_frame_buffer_.reserve(16);
}

}
}

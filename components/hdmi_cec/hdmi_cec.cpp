#include "hdmi_cec.h"
#include "esphome/core/log.h"

#ifdef USE_CEC_DECODER
#include "cec_decoder.h"
#endif

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";
// receiver constants
static const uint32_t START_BIT_MIN_US = 3500;
static const uint32_t HIGH_BIT_MIN_US = 400;
static const uint32_t HIGH_BIT_MAX_US = 800;
// transmitter constants
static const uint32_t TOTAL_BIT_US = 2400;
static const uint32_t HIGH_BIT_US = 600;
static const uint32_t LOW_BIT_US = 1500;
// arbitration and retransmission
static const size_t MAX_ATTEMPTS = 5;

static const gpio::Flags INPUT_MODE_FLAGS = gpio::FLAG_INPUT | gpio::FLAG_PULLUP;
static const gpio::Flags OUTPUT_MODE_FLAGS = gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN;
// Note: the esp8266 does NOT support 'FLAG_OUTPUT | FLAG_OPEN_DRAIN | FLAG_PULLUP' as opposed to the esp32 and rp2040.
// (see 'flags_to_mode' in its esphome gpio.cpp).
// So, unfortunately, in 'OPEN_DRAIN' mode, the required 'PULLUP' cannot be activated.
// Therefor, 'OUTPUT' will be used only to write '0': For writing a '1' the mode is switched to 'INPUT | PULLUP'.
// That allows to safely check for cec bus conflicts on writing '1' (avoid short-circuit with other bus initiators).

Frame::Frame(uint8_t initiator_addr, uint8_t target_addr, const std::vector<uint8_t> &payload)
    : std::vector<uint8_t>(1 + payload.size(), (uint8_t) (0)) {
  this->at(0) = ((initiator_addr & 0xf) << 4) | (target_addr & 0xf);
  std::memcpy(this->data() + 1, payload.data(), payload.size());
}

std::string Frame::to_string(bool skip_decode) const {
  std::string result;
  char part_buffer[3];
  for (auto it = this->cbegin(); it != this->cend(); it++) {
    uint8_t byte_value = *it;
    sprintf(part_buffer, "%02X", byte_value);
    result += part_buffer;

    if (it != (this->end() - 1)) {
      result += ":";
    }
  }
#ifdef USE_CEC_DECODER
  if (!skip_decode) {
    Decoder decoder(*this);
    result += " => " + decoder.decode();
  }
#endif
  return result;
}

inline void IRAM_ATTR HDMICEC::set_pin_input_high() {
  pin_->pin_mode(INPUT_MODE_FLAGS);
}

inline void IRAM_ATTR HDMICEC::set_pin_output_low() {
  pin_->pin_mode(OUTPUT_MODE_FLAGS);
  pin_->digital_write(false);
}

void HDMICEC::setup() {
  this->pin_->setup();
  isr_pin_ = pin_->to_isr();
  frames_queue_.reset();
  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  set_pin_input_high();

  if (negotiation_needed_) {
    negotiate_address_();
    broadcast_physical_address_();
  }

  if (scan_on_boot_ && !monitor_mode_) {
    set_timeout("scan_boot", scan_boot_delay_ms_, [this]() { start_scan(); });
  }
}

void HDMICEC::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC");
  LOG_PIN("  pin: ", pin_);
  ESP_LOGCONFIG(TAG, "  address: 0x%X", address_);
  ESP_LOGCONFIG(TAG, "  promiscuous mode: %s", (promiscuous_mode_ ? "yes" : "no"));
  ESP_LOGCONFIG(TAG, "  monitor mode: %s", (monitor_mode_ ? "yes" : "no"));
}

void HDMICEC::loop() {
  while (const Frame *frame = frames_queue_.front()) {
    uint8_t header = frame->front();
    uint8_t src_addr = ((header & 0xF0) >> 4);
    uint8_t dest_addr = (header & 0x0F);

    if (!promiscuous_mode_ && (dest_addr != 0x0F) && (dest_addr != address_)) {
      // ignore frames not meant for us, recycle frame buffer
      frames_queue_.push_front();
      continue;
    }

    if (frame->size() == 1) {
      // don't process pings. they're already dealt with by the acknowledgement mechanism
      ESP_LOGV(TAG, "ping received: 0x%01X -> 0x%01X", src_addr, dest_addr);
      frames_queue_.push_front();
      continue;
    }

    ESP_LOGD(TAG, "[received] %s", frame->to_string().c_str());

    std::vector<uint8_t> data(frame->begin() + 1, frame->end());

    // recycle received frame buffer
    frames_queue_.push_front();

    // Always update device registry from known response opcodes
    update_device_registry_(src_addr, dest_addr, data);

    // Process on_message triggers
    bool handled_by_trigger = false;
    uint8_t opcode = data[0];
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
        handled_by_trigger = true;
      }
    }

    // If nothing in on_message handled this message, we try to run the built-in handlers
    bool is_directly_addressed = (dest_addr != 0xF && dest_addr == address_);
    if (is_directly_addressed && !handled_by_trigger) {
      try_builtin_handler_(src_addr, dest_addr, data);
    }
  }

  // Send one initial probe per loop iteration during scan
  if (scanning_) {
    for (uint8_t i = 0; i <= MAX_LOGICAL_ADDRESS; i++) {
      if (scan_pending_[i]) {
        scan_pending_[i] = false;
        send(address_, i, {0x83});  // Give Physical Address
        break;
      }
    }
  }
}

uint8_t logical_address_to_device_type(uint8_t logical_address) {
  switch (logical_address) {
    // "TV"
    case 0x0:
      return 0x00; // "TV"

    // "Audio System"
    case 0x5:
      return 0x05; // "Audio System"

    // "Recording 1"
    case 0x1:
    // "Recording 2"
    case 0x2:
    // "Recording 3"
    case 0x9:
      return 0x01; // "Recording Device"

    // "Tuner 1"
    case 0x3:
    // "Tuner 2"
    case 0x6:
    // "Tuner 3"
    case 0x7:
    // "Tuner 4"
    case 0xA:
      return 0x03; // "Tuner"

    default:
      return 0x04; // "Playback Device"
  }
}

static const std::vector<uint8_t> &device_type_to_candidate_addresses(uint8_t device_type) {
  static const std::vector<uint8_t> TV_ADDRS = {0x0};
  static const std::vector<uint8_t> RECORDING_ADDRS = {0x1, 0x2, 0x9};
  static const std::vector<uint8_t> TUNER_ADDRS = {0x3, 0x6, 0x7, 0xA};
  static const std::vector<uint8_t> PLAYBACK_ADDRS = {0x4, 0x8, 0xB};
  static const std::vector<uint8_t> AUDIO_ADDRS = {0x5};
  // Reserved addresses 0xC-0xE: not assigned to any standard device type,
  // so least likely to collide with real CEC hardware.
  static const std::vector<uint8_t> OTHER_ADDRS = {0xE, 0xD, 0xC};
  static const std::vector<uint8_t> EMPTY = {};

  switch (device_type) {
    case 0x00: return TV_ADDRS;
    case 0x01: return RECORDING_ADDRS;
    case 0x03: return TUNER_ADDRS;
    case 0x04: return PLAYBACK_ADDRS;
    case 0x05: return AUDIO_ADDRS;
    case 0xFF: return OTHER_ADDRS;
    default: return EMPTY;
  }
}

bool HDMICEC::test_address_available_(uint8_t candidate_address) {
  // Send a polling message: header-only frame where source == destination
  Frame frame(candidate_address, candidate_address, {});

  // Wait for signal free time (5 bit periods for new initiator)
  int32_t delay = 5 * TOTAL_BIT_US + std::max(last_sent_us_, last_falling_edge_us_) - micros();
  if (delay > 0) {
    delay_microseconds_safe(delay);
  }

  // For a directed message (non-broadcast), send_frame_ returns:
  //   Success if ACKed (another device has this address)
  //   NoAck if not ACKed (address is free)
  auto result = send_frame_(frame, false);
  last_sent_us_ = micros();

  if (result == SendResult::NoAck) {
    ESP_LOGD(TAG, "Address 0x%X is free", candidate_address);
    return true;
  }
  ESP_LOGD(TAG, "Address 0x%X is taken (result=%d)", candidate_address, (int) result);
  return false;
}

void HDMICEC::negotiate_address_() {
  if (!device_type_.has_value()) {
    return;
  }

  ESP_LOGI(TAG, "Starting CEC logical address negotiation");

  address_ = 0x0F;  // Unregistered during negotiation

  const auto &candidates = device_type_to_candidate_addresses(device_type_.value());

  LockGuard send_lock(send_mutex_);

  // First, try the spec-preferred addresses for our device type
  for (uint8_t candidate : candidates) {
    if (test_address_available_(candidate)) {
      address_ = candidate;
      ESP_LOGI(TAG, "Claimed logical address 0x%X", address_);
      return;
    }
  }

  // All preferred addresses taken. Fall back to any available address,
  // starting from 0xE (reserved/rarely used) down through standard addresses.
  // Skip 0xF (broadcast) and any we already tried above.
  ESP_LOGI(TAG, "Preferred addresses taken, trying fallback addresses");
  for (int8_t candidate = 0xE; candidate >= 0; candidate--) {
    if (test_address_available_(candidate)) {
      address_ = candidate;
      ESP_LOGI(TAG, "Claimed fallback logical address 0x%X", address_);
      return;
    }
  }

  ESP_LOGW(TAG, "All addresses taken, using Unregistered (0xF)");
}

void HDMICEC::broadcast_physical_address_() {
  auto physical_address_bytes = decode_value(physical_address_);
  std::vector<uint8_t> data = {0x84};
  data.insert(data.end(), physical_address_bytes.begin(), physical_address_bytes.end());
  data.push_back(logical_address_to_device_type(address_));
  send(address_, 0xF, data);
}

// --- Bus scanning ---

void HDMICEC::start_scan() {
  if (monitor_mode_) {
    ESP_LOGW(TAG, "Cannot scan in monitor mode");
    return;
  }
  cancel_timeout("scan_complete");
  scanning_ = true;
  last_scan_start_ms_ = millis();
  for (uint8_t i = 0; i <= MAX_LOGICAL_ADDRESS; i++) {
    scan_pending_[i] = (i != address_);
  }
  ESP_LOGI(TAG, "Starting CEC bus scan");
  set_timeout("scan_complete", 10000, [this]() { complete_scan_(); });
}

void HDMICEC::complete_scan_() {
  scanning_ = false;
  ESP_LOGI(TAG, "CEC bus scan complete");
  for (auto *trigger : scan_complete_triggers_) {
    trigger->trigger();
  }
}

void HDMICEC::update_device_registry_(uint8_t src, uint8_t dest, const std::vector<uint8_t> &data) {
  if (data.empty() || src > MAX_LOGICAL_ADDRESS) return;

  auto &dev = device_registry_[src];
  dev.last_seen_ms = millis();

  uint8_t opcode = data[0];
  switch (opcode) {
    case 0x84:  // Report Physical Address: [0x84, phys_hi, phys_lo, dev_type]
      if (data.size() >= 4) {
        dev.physical_address = (uint16_t(data[1]) << 8) | data[2];
        dev.device_type = data[3];
        ESP_LOGD(TAG, "Device 0x%X: physical_address=%04X device_type=%d", src, dev.physical_address, dev.device_type);
      }
      break;
    case 0x47:  // Set OSD Name: [0x47, name_bytes...]
      if (data.size() >= 2) {
        dev.osd_name = std::string(data.begin() + 1, data.end());
        ESP_LOGD(TAG, "Device 0x%X: osd_name='%s'", src, dev.osd_name.c_str());
      }
      break;
    case 0x87:  // Device Vendor ID: [0x87, v1, v2, v3]
      if (data.size() >= 4) {
        dev.vendor_id = (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
        ESP_LOGD(TAG, "Device 0x%X: vendor_id=%06X", src, dev.vendor_id);
      }
      break;
    case 0x90:  // Report Power Status: [0x90, status]
      if (data.size() >= 2) {
        dev.power_status = data[1];
        ESP_LOGD(TAG, "Device 0x%X: power_status=%d", src, dev.power_status);
      }
      break;
    case 0x9E:  // CEC Version: [0x9E, version]
      if (data.size() >= 2) {
        dev.cec_version = data[1];
        ESP_LOGD(TAG, "Device 0x%X: cec_version=%d", src, dev.cec_version);
      }
      break;
    case 0x82:  // Active Source: [0x82, phys_hi, phys_lo] (broadcast)
      if (data.size() >= 3) {
        for (auto &d : device_registry_) d.active_source = false;
        dev.active_source = true;
        dev.physical_address = (uint16_t(data[1]) << 8) | data[2];
        ESP_LOGD(TAG, "Device 0x%X: active_source, physical_address=%04X", src, dev.physical_address);
      }
      break;
    default:
      return;  // not a registry-relevant opcode
  }

  // During a scan, chain the next query based on the response we just got
  if (scanning_) {
    switch (opcode) {
      case 0x84: send(address_, src, {0x8C}); break;  // → Give Device Vendor ID
      case 0x87: send(address_, src, {0x46}); break;  // → Give OSD Name
      case 0x47: send(address_, src, {0x8F}); break;  // → Give Device Power Status
      case 0x90: send(address_, src, {0x9F}); break;  // → Get CEC Version
      // 0x9E: last in chain, no follow-up
      // 0x82: passive only, no chain
    }
  }
}

const DeviceInfo &HDMICEC::get_device_info(uint8_t logical_address) const {
  static const DeviceInfo EMPTY{};
  if (logical_address > MAX_LOGICAL_ADDRESS) return EMPTY;
  return device_registry_[logical_address];
}

bool HDMICEC::seen_on_last_scan(uint8_t logical_address) const {
  if (logical_address > MAX_LOGICAL_ADDRESS || last_scan_start_ms_ == 0) return false;
  return device_registry_[logical_address].last_seen_ms >= last_scan_start_ms_;
}

optional<uint8_t> HDMICEC::find_address_by_osd_name(const std::string &name) const {
  for (uint8_t i = 0; i <= MAX_LOGICAL_ADDRESS; i++) {
    if (device_registry_[i].last_seen_ms > 0 && device_registry_[i].osd_name == name) {
      return i;
    }
  }
  return {};
}

optional<uint8_t> HDMICEC::find_address_by_physical_address(uint16_t addr) const {
  for (uint8_t i = 0; i <= MAX_LOGICAL_ADDRESS; i++) {
    if (device_registry_[i].last_seen_ms > 0 && device_registry_[i].physical_address == addr) {
      return i;
    }
  }
  return {};
}

optional<uint8_t> HDMICEC::find_address_by_vendor_and_type(uint32_t vendor_id, uint8_t device_type) const {
  for (uint8_t i = 0; i <= MAX_LOGICAL_ADDRESS; i++) {
    if (device_registry_[i].last_seen_ms > 0 &&
        device_registry_[i].vendor_id == vendor_id &&
        device_registry_[i].device_type == device_type) {
      return i;
    }
  }
  return {};
}

void HDMICEC::try_builtin_handler_(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return;
  }

  uint8_t opcode = data[0];
  switch (opcode) {
    // "Get CEC Version" request
    case 0x9F: {
      // reply with "CEC Version" (0x9E)
      send(address_, source, {0x9E, 0x04});
      break;
    }

    // "Give Device Power Status" request
    case 0x8F: {
      // reply with "Report Power Status" (0x90)
      send(address_, source, {0x90, 0x00}); // "On"
      break;
    }

    // "Give OSD Name" request
    case 0x46: {
      // reply with "Set OSD Name" (0x47)
      std::vector<uint8_t> data = { 0x47 };
      data.insert(data.end(), osd_name_bytes_.begin(), osd_name_bytes_.end());
      send(address_, source, data);
      break;
    }

    // "Give Physical Address" request
    case 0x83: {
      // reply with "Report Physical Address" (0x84)
      auto physical_address_bytes = decode_value(physical_address_);
      std::vector<uint8_t> data = { 0x84 };
      data.insert(data.end(), physical_address_bytes.begin(), physical_address_bytes.end());
      // Device Type
      data.push_back(logical_address_to_device_type(address_));
      // Broadcast Physical Address
      send(address_, 0xF, data);
      break;
    }

    // Ignore "Feature Abort" opcode responses
    case 0x00:
    // Response opcodes handled by update_device_registry_ - don't Feature Abort them
    case 0x82:  // Active Source
    case 0x84:  // Report Physical Address
    case 0x47:  // Set OSD Name
    case 0x87:  // Device Vendor ID
    case 0x90:  // Report Power Status
    case 0x9E:  // CEC Version
      // no-op
      break;

    // default case (no built-in handler + no on_message handler) => message not supported => send "Feature Abort"
    default:
      send(address_, source, {0x00, opcode, 0x00});
      break;
  }
}

bool HDMICEC::send(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data_bytes) {
  if (monitor_mode_) return false;

  bool is_broadcast = (destination == 0xF);

  // prepare the bytes to send
  Frame frame(source, destination, data_bytes);
  ESP_LOGD(TAG, "[sending] %s", frame.to_string().c_str());

  {
    LockGuard send_lock(send_mutex_);
    // Bus 'Signal Free' time between transmissions, according to the HDMI-CEC standard, shall be a minimum of:
    //  - 7 bit periods between successive transmissions of same sender
    //  - 5 bit periods between transmissions of different senders
    //  - 3 bit periods for resend of a failed transmission attempt
    uint8_t free_bit_periods = (last_sent_us_ > last_falling_edge_us_) ? 7 : 5;

    for (size_t i = 0; i < MAX_ATTEMPTS; i++) {
      int32_t delay = 0;
      while ((delay = free_bit_periods * TOTAL_BIT_US + std::max(last_sent_us_, last_falling_edge_us_) - micros()) > 0) {
        ESP_LOGV(TAG, "HDMICEC::send(): waiting %d usec for bus free period", delay);
        delay_microseconds_safe(delay);
        // Note: during this delay, the 'last_falling_edge_us_' might be incremented by 'gpio_intr_', requiring further wait
        free_bit_periods = 5;
      }
      ESP_LOGV(TAG, "HDMICEC::send(): bus available, sending frame...");

      auto result = send_frame_(frame, is_broadcast);
      if (result == SendResult::Success) {
        ESP_LOGD(TAG, "frame sent and acknowledged");
        return true;
      }
      ESP_LOGI(TAG, "HDMICEC::send(): frame not sent: %s",
               ((result == SendResult::BusCollision) ? "Bus Collision" : "No Ack received"));
      // attempt retransmission with smaller free time gap
      free_bit_periods = 3;
    }
  }

  ESP_LOGE(TAG, "HDMICEC::send(): send failed after five attempts");
  return false;
}

SendResult IRAM_ATTR HDMICEC::send_frame_(const Frame &frame, bool is_broadcast) {
  pin_->detach_interrupt();  // do NOT listen for pin changes while sending
  auto result = SendResult::Success;

  bool success = send_start_bit_();

  // for each byte of the frame:
  for (auto it = frame.begin(); it != frame.end(); ++it) {
    uint8_t current_byte = *it;

    // 1. send the current byte
    for (int8_t i = 7; (i >= 0) && success; i--) {
      bool bit_value = ((current_byte >> i) & 0b1);
      if ((it == frame.begin()) && i >= 4 && bit_value) {
        // my initiator address bit is 1: test for bus collision
        // see the specification in the HDMI standard, section "CEC Arbitration"
        success = send_high_and_test_();
      } else {
        send_bit_(bit_value);
      }
    }

    if (!success) {
      // immediatly stop sending bits due to bus collision:
      // the other concurrent initiator with lower address might not have detected the conflict
      result = SendResult::BusCollision;
      break;
    }

    // 2. send EOM bit (logic 1 if this is the last byte of the frame)
    bool is_eom = (it == (frame.end() - 1));
    send_bit_(is_eom);

    // 3. send ack bit and test bit value from destination(s)
    bool value = send_high_and_test_();
    success = (value == is_broadcast);  // 'no broadcast' should give a 'false' signal value as 'ack'
    if (!success) {
      result = SendResult::NoAck;
      break;
    }
  }
  // capture last bus busy time also for bus writes (with interrupts off)
  last_sent_us_ = micros();
  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  return result;
}

bool IRAM_ATTR HDMICEC::send_start_bit_() {
  // 1. pull low for 3700 us
  set_pin_output_low();
  delay_microseconds_safe(3700);

  // 2. pull high for 800 us
  set_pin_input_high();
  delay_microseconds_safe(400);

  // check half-way the 'high' interval for no collision
  bool value = pin_->digital_read();

  // check at end of 'high' interval for no collision
  delay_microseconds_safe(400);
  value &= pin_->digital_read();

  // total duration of start bit: 4500 us
  // No other initiator tried to 'start' concurrently by pulling the pin low?
  bool success = (value == true);
  return success;
}

void IRAM_ATTR HDMICEC::send_bit_(bool bit_value) {
  // total bit duration:
  // logic 1: pull low for 600 us, then pull high for 1800 us
  // logic 0: pull low for 1500 us, then pull high for 900 us

  const uint32_t low_duration_us = (bit_value ? HIGH_BIT_US : LOW_BIT_US);
  const uint32_t high_duration_us = (TOTAL_BIT_US - low_duration_us);

  set_pin_output_low();
  delay_microseconds_safe(low_duration_us);
  set_pin_input_high();
  delay_microseconds_safe(high_duration_us);
}

bool IRAM_ATTR HDMICEC::send_high_and_test_() {
  uint32_t start_us = micros();

  // send a Logical 1
  set_pin_output_low();
  delay_microseconds_safe(HIGH_BIT_US);
  set_pin_input_high();

  // ...then wait up to the middle of the "Safe sample period" (CEC spec -> Signaling and Bit Timing -> Figure 5)
  static const uint32_t SAFE_SAMPLE_US = 1050;
  delay_microseconds_safe(SAFE_SAMPLE_US - (micros() - start_us));
  bool value = pin_->digital_read();

  // sleep for the rest of the bit period
  delay_microseconds_safe(TOTAL_BIT_US - (micros() - start_us));

  // If a 'high' value was read, the 'low' pulse was short, not lengthened by another driver.
  // Such short pulse represents a 'high' bit.
  return value;
}

void IRAM_ATTR HDMICEC::gpio_intr_(HDMICEC *self) {
  const uint32_t now = micros();
  const bool level = self->isr_pin_.digital_read();

  if (level == self->last_level_) {
    // spurious interrupt, probably resulting from a pin mode change
    return;
  }
  self->last_level_ = level;

  // on falling edge, store current time as the start of the low pulse
  if (level == false) {
    self->last_falling_edge_us_ = now;

    if (self->recv_ack_queued_ && !self->monitor_mode_) {
      self->recv_ack_queued_ = false;
      {
        InterruptLock interrupt_lock;
        self->set_pin_output_low();
        delay_microseconds_safe(LOW_BIT_US);
        self->set_pin_input_high();
      }
    }
    return;
  }
  // otherwise, it's a rising edge, so it's time to process the pulse length

  auto pulse_duration = (now - self->last_falling_edge_us_);

  if (pulse_duration > START_BIT_MIN_US) {
    // start bit detected. reset everything and start receiving
    self->receiver_state_ = ReceiverState::ReceivingByte;
    reset_state_variables_(self);
    self->recv_ack_queued_ = false;
    // pick frame receive buffer to fill, if available.
    self->frame_receive_ = self->frames_queue_.back();
    return;
  } else if (pulse_duration < (HIGH_BIT_MIN_US / 4)) {
    // short glitch on the line: ignore
    return;
  }

  bool value = (pulse_duration >= HIGH_BIT_MIN_US && pulse_duration <= HIGH_BIT_MAX_US);
  
  switch (self->receiver_state_) {
    case ReceiverState::ReceivingByte: {
      // write bit to the current byte
      self->recv_byte_buffer_ = (self->recv_byte_buffer_ << 1) | (value & 0b1);

      self->recv_bit_counter_++;
      if (self->recv_bit_counter_ >= 8) { 
        // if we reached eight bits, push the current byte to the frame buffer
        if (self->frame_receive_) {
          self->frame_receive_->push_back(self->recv_byte_buffer_);
        }

        self->recv_bit_counter_ = 0;
        self->recv_byte_buffer_ = 0;

        self->receiver_state_ = ReceiverState::WaitingForEOM;
      } else {
        self->receiver_state_ = ReceiverState::ReceivingByte;
      }
      break;
    }

    case ReceiverState::WaitingForEOM: {
      // check if we need to acknowledge this byte on the next bit
      uint8_t destination_address = self->frame_receive_ ? (self->frame_receive_->front() & 0x0F) : 0xF;
      if (destination_address != 0xF && destination_address == self->address_) {
        self->recv_ack_queued_ = true;
      }

      bool isEOM = (value == 1);
      if (isEOM) {
        // pass frame to app
        if (self->frame_receive_ && self->frame_receive_->size() > 0) {
          self->frames_queue_.push_back();
          self->frame_receive_ = nullptr;
        }
        reset_state_variables_(self);
      }

      self->receiver_state_ = (
        isEOM
        ? ReceiverState::WaitingForEOMAck
        : ReceiverState::WaitingForAck
      );
      break;
    }

    case ReceiverState::WaitingForAck: {
      self->receiver_state_ = ReceiverState::ReceivingByte;
      break;
    }

    case ReceiverState::WaitingForEOMAck: {
      self->receiver_state_ = ReceiverState::Idle;
      break;
    }

    default: {
      break;
    }
  }
}

void IRAM_ATTR HDMICEC::reset_state_variables_(HDMICEC *self) {
  self->recv_bit_counter_ = 0;
  self->recv_byte_buffer_ = 0x0;
}

}
}

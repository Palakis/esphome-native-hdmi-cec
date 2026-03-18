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

// Scan query chain: each entry is {request_opcode, response_opcode}.
// During a bus scan, we send queries in this order and chain the next
// query when we receive the expected response.
struct ScanQuery {
  uint8_t request;
  uint8_t response;
};
static const ScanQuery SCAN_QUERIES[] = {
    {CEC_OPCODE_GIVE_PHYSICAL_ADDRESS, CEC_OPCODE_REPORT_PHYSICAL_ADDRESS},
    {CEC_OPCODE_GIVE_DEVICE_VENDOR_ID, CEC_OPCODE_DEVICE_VENDOR_ID},
    {CEC_OPCODE_GET_CEC_VERSION, CEC_OPCODE_CEC_VERSION},
    {CEC_OPCODE_GIVE_OSD_NAME, CEC_OPCODE_SET_OSD_NAME},
    {CEC_OPCODE_GIVE_DEVICE_POWER_STATUS, CEC_OPCODE_REPORT_POWER_STATUS},
};
static const size_t SCAN_QUERY_COUNT = sizeof(SCAN_QUERIES) / sizeof(SCAN_QUERIES[0]);

static bool is_scan_response_opcode(uint8_t opcode) {
  for (size_t i = 0; i < SCAN_QUERY_COUNT; i++) {
    if (opcode == SCAN_QUERIES[i].response)
      return true;
  }
  return false;
}

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

inline void IRAM_ATTR HDMICEC::set_pin_input_high() { pin_->pin_mode(INPUT_MODE_FLAGS); }

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

  if (address_sensor_ != nullptr) {
    char buf[5];
    sprintf(buf, "0x%X", address_);
    address_sensor_->publish_state(buf);
  }

  if (scan_on_boot_ && !monitor_mode_) {
    set_timeout("scan_boot", scan_boot_delay_ms_, [this]() { start_scan(); });
  }

#ifdef USE_API_CUSTOM_SERVICES
  register_service(&HDMICEC::on_send_, "send", {"destination", "data"});
  register_service(&HDMICEC::on_send_to_osd_name_, "send_to_osd_name", {"osd_name", "data"});
  register_service(&HDMICEC::on_send_to_physical_address_, "send_to_physical_address", {"physical_address", "data"});
  register_service(&HDMICEC::on_send_to_vendor_and_type_, "send_to_vendor_and_type",
                   {"vendor_id", "device_type", "data"});
  register_service(&HDMICEC::on_scan_bus_, "scan_bus");
#elif defined(USE_API)
#warning \
    "hdmi_cec: HA services (send, scan_bus, etc.) are disabled. Add 'custom_services: true' to your 'api:' config to enable them."
#endif
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

    if (!promiscuous_mode_ && (dest_addr != CEC_ADDR_BROADCAST) && (dest_addr != address_)) {
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
    update_devices_(src_addr, dest_addr, data);

    // Process on_message triggers
    bool handled_by_trigger = false;
    uint8_t opcode = data[0];
    for (auto trigger : message_triggers_) {
      bool can_trigger =
          ((!trigger->source_.has_value() || (trigger->source_ == src_addr)) &&
           (!trigger->destination_.has_value() || (trigger->destination_ == dest_addr)) &&
           (!trigger->opcode_.has_value() || (trigger->opcode_ == opcode)) &&
           (!trigger->data_.has_value() || (data.size() == trigger->data_->size() &&
                                            std::equal(trigger->data_->begin(), trigger->data_->end(), data.begin()))));
      if (can_trigger) {
        trigger->trigger(src_addr, dest_addr, data);
        handled_by_trigger = true;
      }
    }

    // If nothing in on_message handled this message, we try to run the built-in handlers
    bool is_directly_addressed = (dest_addr != CEC_ADDR_BROADCAST && dest_addr == address_);
    if (is_directly_addressed && !handled_by_trigger) {
      try_builtin_handler_(src_addr, dest_addr, data);
    }
  }

  // Send one initial probe per loop iteration during scan
  if (scanning_) {
    for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
      if (scan_pending_[i]) {
        scan_pending_[i] = false;
        send(address_, i, {SCAN_QUERIES[0].request});
        break;
      }
    }
  }
}

uint8_t logical_address_to_device_type(uint8_t logical_address) {
  switch (logical_address) {
    case CEC_ADDR_TV:
      return CEC_DEVICE_TYPE_TV;
    case CEC_ADDR_AUDIO_SYSTEM:
      return CEC_DEVICE_TYPE_AUDIO_SYSTEM;
    case CEC_ADDR_RECORDING_1:
    case CEC_ADDR_RECORDING_2:
    case CEC_ADDR_RECORDING_3:
      return CEC_DEVICE_TYPE_RECORDING;
    case CEC_ADDR_TUNER_1:
    case CEC_ADDR_TUNER_2:
    case CEC_ADDR_TUNER_3:
    case CEC_ADDR_TUNER_4:
      return CEC_DEVICE_TYPE_TUNER;
    case CEC_ADDR_PLAYBACK_1:
    case CEC_ADDR_PLAYBACK_2:
    case CEC_ADDR_PLAYBACK_3:
      return CEC_DEVICE_TYPE_PLAYBACK;
    default:
      return CEC_DEVICE_TYPE_OTHER;
  }
}

static const std::vector<uint8_t> &device_type_to_candidate_addresses(uint8_t device_type) {
  static const std::vector<uint8_t> TV_ADDRS = {CEC_ADDR_TV};
  static const std::vector<uint8_t> RECORDING_ADDRS = {CEC_ADDR_RECORDING_1, CEC_ADDR_RECORDING_2,
                                                       CEC_ADDR_RECORDING_3};
  static const std::vector<uint8_t> TUNER_ADDRS = {CEC_ADDR_TUNER_1, CEC_ADDR_TUNER_2, CEC_ADDR_TUNER_3,
                                                   CEC_ADDR_TUNER_4};
  static const std::vector<uint8_t> PLAYBACK_ADDRS = {CEC_ADDR_PLAYBACK_1, CEC_ADDR_PLAYBACK_2, CEC_ADDR_PLAYBACK_3};
  static const std::vector<uint8_t> AUDIO_ADDRS = {CEC_ADDR_AUDIO_SYSTEM};
  static const std::vector<uint8_t> OTHER_ADDRS = {CEC_ADDR_FREE_USE, CEC_ADDR_RESERVED_2, CEC_ADDR_RESERVED_1};

  switch (device_type) {
    case CEC_DEVICE_TYPE_TV:
      return TV_ADDRS;
    case CEC_DEVICE_TYPE_RECORDING:
      return RECORDING_ADDRS;
    case CEC_DEVICE_TYPE_TUNER:
      return TUNER_ADDRS;
    case CEC_DEVICE_TYPE_PLAYBACK:
      return PLAYBACK_ADDRS;
    case CEC_DEVICE_TYPE_AUDIO_SYSTEM:
      return AUDIO_ADDRS;
    default:
      return OTHER_ADDRS;
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

  address_ = CEC_ADDR_BROADCAST;  // Unregistered during negotiation

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
  for (int8_t candidate = CEC_ADDR_FREE_USE; candidate >= 0; candidate--) {
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
  std::vector<uint8_t> data = {CEC_OPCODE_REPORT_PHYSICAL_ADDRESS};
  data.insert(data.end(), physical_address_bytes.begin(), physical_address_bytes.end());
  data.push_back(logical_address_to_device_type(address_));
  send(address_, CEC_ADDR_BROADCAST, data);
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
  for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
    scan_pending_[i] = (i != address_);
  }
  ESP_LOGI(TAG, "Starting CEC bus scan");
  set_timeout("scan_complete", 10000, [this]() { complete_scan_(); });
}

void HDMICEC::complete_scan_() {
  scanning_ = false;
  uint8_t count = 0;
  ESP_LOGI(TAG, "=== CEC Bus Scan Results ===");
  for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
    if (devices_[i].last_seen_ms < last_scan_start_ms_)
      continue;
    count++;
    auto &dev = devices_[i];
    auto vendor_str = CECDevice::vendor_id_to_string(dev.vendor_id);
    auto vendor_name = CECDevice::vendor_name_to_string(dev.vendor_id);
    if (vendor_name != "Unknown")
      vendor_str += " (" + vendor_name + ")";
    ESP_LOGI(TAG, "  0x%X: %s, %s, vendor %s, CEC %s, %s%s%s", i,
             CECDevice::device_type_to_string(dev.device_type).c_str(),
             CECDevice::physical_address_to_string(dev.physical_address).c_str(), vendor_str.c_str(),
             CECDevice::cec_version_to_string(dev.cec_version).c_str(),
             CECDevice::power_status_to_string(dev.power_status).c_str(), dev.osd_name.empty() ? "" : ", OSD \"",
             dev.osd_name.empty() ? "" : (dev.osd_name + "\"").c_str());
  }
  ESP_LOGI(TAG, "=== %d device(s) found ===", count);
  for (auto *trigger : scan_complete_triggers_) {
    trigger->trigger();
  }
}

void HDMICEC::update_devices_(uint8_t src, uint8_t dest, const std::vector<uint8_t> &data) {
  if (data.empty() || src > CEC_ADDR_MAX_ASSIGNABLE)
    return;

  auto &dev = devices_[src];
  dev.logical_address = src;
  dev.last_seen_ms = millis();

  uint8_t opcode = data[0];
  switch (opcode) {
    case CEC_OPCODE_REPORT_PHYSICAL_ADDRESS:
      if (data.size() >= 4) {
        dev.physical_address = (uint16_t(data[1]) << 8) | data[2];
        dev.device_type = data[3];
        ESP_LOGD(TAG, "Device 0x%X: physical_address=%04X device_type=%d", src, dev.physical_address, dev.device_type);
      }
      break;
    case CEC_OPCODE_SET_OSD_NAME:
      if (data.size() >= 2) {
        dev.osd_name = std::string(data.begin() + 1, data.end());
        ESP_LOGD(TAG, "Device 0x%X: osd_name='%s'", src, dev.osd_name.c_str());
      }
      break;
    case CEC_OPCODE_DEVICE_VENDOR_ID:
      if (data.size() >= 4) {
        dev.vendor_id = (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
        ESP_LOGD(TAG, "Device 0x%X: vendor_id=%06X", src, dev.vendor_id);
      }
      break;
    case CEC_OPCODE_REPORT_POWER_STATUS:
      if (data.size() >= 2) {
        dev.power_status = data[1];
        ESP_LOGD(TAG, "Device 0x%X: power_status=%d", src, dev.power_status);
      }
      break;
    case CEC_OPCODE_CEC_VERSION:
      if (data.size() >= 2) {
        dev.cec_version = data[1];
        ESP_LOGD(TAG, "Device 0x%X: cec_version=%d", src, dev.cec_version);
      }
      break;
    case CEC_OPCODE_ACTIVE_SOURCE:
      if (data.size() >= 3) {
        for (auto &d : devices_) d.active_source = false;
        dev.active_source = true;
        dev.physical_address = (uint16_t(data[1]) << 8) | data[2];
        ESP_LOGD(TAG, "Device 0x%X: active_source, physical_address=%04X", src, dev.physical_address);
        if (active_source_sensor_ != nullptr) {
          char buf[5];
          sprintf(buf, "0x%X", src);
          active_source_sensor_->publish_state(buf);
        }
      }
      break;
    case CEC_OPCODE_FEATURE_ABORT:
      break;
    default:
      return;
  }

  // During a scan, chain the next query based on the response we just got.
  // Feature Abort contains the rejected request opcode in data[1], so we can
  // match it against SCAN_QUERIES[i].request to skip to the next query.
  if (scanning_) {
    for (size_t i = 0; i + 1 < SCAN_QUERY_COUNT; i++) {
      bool is_response = (opcode == SCAN_QUERIES[i].response);
      bool is_abort = (opcode == CEC_OPCODE_FEATURE_ABORT && data.size() >= 2 && data[1] == SCAN_QUERIES[i].request);
      if (is_response || is_abort) {
        send(address_, src, {SCAN_QUERIES[i + 1].request});
        break;
      }
    }
  }
}

const CECDevice &HDMICEC::get_device(uint8_t logical_address) const {
  static const CECDevice EMPTY{};
  if (logical_address > CEC_ADDR_MAX_ASSIGNABLE)
    return EMPTY;
  return devices_[logical_address];
}

bool HDMICEC::seen_on_last_scan(uint8_t logical_address) const {
  if (logical_address > CEC_ADDR_MAX_ASSIGNABLE || last_scan_start_ms_ == 0)
    return false;
  return devices_[logical_address].last_seen_ms >= last_scan_start_ms_;
}

optional<uint8_t> HDMICEC::find_address_by_osd_name(const std::string &name) const {
  optional<uint8_t> best;
  uint32_t best_seen = 0;
  for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
    if (devices_[i].last_seen_ms > 0 && devices_[i].osd_name == name && devices_[i].last_seen_ms >= best_seen) {
      best = i;
      best_seen = devices_[i].last_seen_ms;
    }
  }
  return best;
}

optional<uint8_t> HDMICEC::find_address_by_physical_address(uint16_t addr) const {
  optional<uint8_t> best;
  uint32_t best_seen = 0;
  for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
    if (devices_[i].last_seen_ms > 0 && devices_[i].physical_address == addr && devices_[i].last_seen_ms >= best_seen) {
      best = i;
      best_seen = devices_[i].last_seen_ms;
    }
  }
  return best;
}

optional<uint8_t> HDMICEC::find_address_by_vendor_and_type(uint32_t vendor_id, uint8_t device_type) const {
  optional<uint8_t> best;
  uint32_t best_seen = 0;
  for (uint8_t i = 0; i <= CEC_ADDR_MAX_ASSIGNABLE; i++) {
    if (devices_[i].last_seen_ms > 0 && devices_[i].vendor_id == vendor_id && devices_[i].device_type == device_type &&
        devices_[i].last_seen_ms >= best_seen) {
      best = i;
      best_seen = devices_[i].last_seen_ms;
    }
  }
  return best;
}

bool HDMICEC::send_to_osd_name(uint8_t source, const std::string &name, const std::vector<uint8_t> &data) {
  auto addr = find_address_by_osd_name(name);
  if (addr.has_value()) {
    return send(source, *addr, data);
  }
  ESP_LOGW(TAG, "send_to_osd_name: no device found with name '%s'", name.c_str());
  return false;
}

bool HDMICEC::send_to_physical_address(uint8_t source, uint16_t phys_addr, const std::vector<uint8_t> &data) {
  auto addr = find_address_by_physical_address(phys_addr);
  if (addr.has_value()) {
    return send(source, *addr, data);
  }
  ESP_LOGW(TAG, "send_to_physical_address: no device found with address 0x%04X", phys_addr);
  return false;
}

bool HDMICEC::send_to_vendor_and_type(uint8_t source, uint32_t vendor_id, uint8_t device_type,
                                      const std::vector<uint8_t> &data) {
  auto addr = find_address_by_vendor_and_type(vendor_id, device_type);
  if (addr.has_value()) {
    return send(source, *addr, data);
  }
  ESP_LOGW(TAG, "send_to_vendor_and_type: no device found with vendor 0x%06X type %d", vendor_id, device_type);
  return false;
}

void HDMICEC::try_builtin_handler_(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return;
  }

  uint8_t opcode = data[0];
  switch (opcode) {
    case CEC_OPCODE_GET_CEC_VERSION: {
      send(address_, source, {CEC_OPCODE_CEC_VERSION, CEC_VERSION_1_3A});
      break;
    }

    case CEC_OPCODE_GIVE_DEVICE_POWER_STATUS: {
      send(address_, source, {CEC_OPCODE_REPORT_POWER_STATUS, CEC_POWER_STATUS_ON});
      break;
    }

    case CEC_OPCODE_GIVE_OSD_NAME: {
      std::vector<uint8_t> data = {CEC_OPCODE_SET_OSD_NAME};
      data.insert(data.end(), osd_name_bytes_.begin(), osd_name_bytes_.end());
      send(address_, source, data);
      break;
    }

    case CEC_OPCODE_GIVE_PHYSICAL_ADDRESS: {
      auto physical_address_bytes = decode_value(physical_address_);
      std::vector<uint8_t> data = {CEC_OPCODE_REPORT_PHYSICAL_ADDRESS};
      data.insert(data.end(), physical_address_bytes.begin(), physical_address_bytes.end());
      data.push_back(logical_address_to_device_type(address_));
      send(address_, CEC_ADDR_BROADCAST, data);
      break;
    }

    case CEC_OPCODE_FEATURE_ABORT:
    case CEC_OPCODE_ACTIVE_SOURCE:
      break;

    default:
      if (is_scan_response_opcode(opcode))
        break;
      send(address_, source, {CEC_OPCODE_FEATURE_ABORT, opcode, CEC_ABORT_UNRECOGNIZED_OPCODE});
      break;
  }
}

bool HDMICEC::send(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data_bytes) {
  if (monitor_mode_)
    return false;

  bool is_broadcast = (destination == CEC_ADDR_BROADCAST);

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
      while ((delay = free_bit_periods * TOTAL_BIT_US + std::max(last_sent_us_, last_falling_edge_us_) - micros()) >
             0) {
        ESP_LOGV(TAG, "HDMICEC::send(): waiting %d usec for bus free period", delay);
        delay_microseconds_safe(delay);
        // Note: during this delay, the 'last_falling_edge_us_' might be incremented by 'gpio_intr_', requiring further
        // wait
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
      uint8_t destination_address = self->frame_receive_ ? (self->frame_receive_->front() & 0x0F) : CEC_ADDR_BROADCAST;
      if (destination_address != CEC_ADDR_BROADCAST && destination_address == self->address_) {
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

      self->receiver_state_ = (isEOM ? ReceiverState::WaitingForEOMAck : ReceiverState::WaitingForAck);
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

// --- CECDevice static methods ---

std::string CECDevice::device_type_to_string(uint8_t type) {
#ifdef USE_CEC_DECODER
  const char *name = Decoder::find_device_type_name(type);
  if (name != nullptr)
    return name;
#endif
  return "Unknown";
}

std::string CECDevice::physical_address_to_string(uint16_t addr) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", (addr >> 12) & 0xF, (addr >> 8) & 0xF, (addr >> 4) & 0xF, addr & 0xF);
  return buf;
}

std::string CECDevice::vendor_id_to_string(uint32_t id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "0x%06X", id);
  return buf;
}

std::string CECDevice::vendor_name_to_string(uint32_t id) {
#ifdef USE_CEC_DECODER
  const char *name = Decoder::find_vendor_name(id);
  if (name != nullptr)
    return name;
#endif
  return "Unknown";
}

std::string CECDevice::cec_version_to_string(uint8_t version) {
#ifdef USE_CEC_DECODER
  const char *name = Decoder::find_cec_version_name(version);
  if (name != nullptr)
    return name;
#endif
  return "Unknown";
}

std::string CECDevice::power_status_to_string(uint8_t status) {
#ifdef USE_CEC_DECODER
  const char *name = Decoder::find_power_status_name(status);
  if (name != nullptr)
    return name;
#endif
  return "Unknown";
}

#ifdef USE_API_CUSTOM_SERVICES
static std::vector<uint8_t> ints_to_bytes(const std::vector<int32_t> &data) {
  return std::vector<uint8_t>(data.begin(), data.end());
}

void HDMICEC::on_send_(int32_t destination, std::vector<int32_t> data) {
  send(address_, static_cast<uint8_t>(destination), ints_to_bytes(data));
}

void HDMICEC::on_send_to_osd_name_(std::string osd_name, std::vector<int32_t> data) {
  send_to_osd_name(address_, osd_name, ints_to_bytes(data));
}

void HDMICEC::on_send_to_physical_address_(int32_t physical_address, std::vector<int32_t> data) {
  send_to_physical_address(address_, static_cast<uint16_t>(physical_address), ints_to_bytes(data));
}

void HDMICEC::on_send_to_vendor_and_type_(int32_t vendor_id, int32_t device_type, std::vector<int32_t> data) {
  send_to_vendor_and_type(address_, static_cast<uint32_t>(vendor_id), static_cast<uint8_t>(device_type),
                          ints_to_bytes(data));
}

void HDMICEC::on_scan_bus_() { start_scan(); }
#endif

}  // namespace hdmi_cec
}  // namespace esphome

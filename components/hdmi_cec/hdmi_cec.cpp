#include "hdmi_cec.h"
#include "esphome/core/log.h"

#include <algorithm>

#ifdef USE_CEC_DECODER
#include "cec_decoder.h"
#endif

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";

// Where the decoder runs in a task it does not belong in IRAM.
#ifdef HDMI_CEC_USE_FREERTOS
#define HDMI_CEC_RX_ATTR
#else
#define HDMI_CEC_RX_ATTR IRAM_ATTR
#endif
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
// Yield interval for bus-free wait loop: break long waits into chunks of this
// duration and call yield() between each, so the FreeRTOS scheduler can run
// other tasks and the Task Watchdog Timer is not triggered.
// 1ms is a good trade-off: short enough to maintain CEC timing accuracy
// (bus-free periods are 7200-16800µs), yet long enough to avoid excessive
// context-switch overhead from yielding on every microsecond-scale iteration.
static const uint32_t YIELD_INTERVAL_US = 1000;

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
  tx_results_.reserve(MAX_FRAMES_TO_SEND);

#ifdef HDMI_CEC_USE_FREERTOS
  tx_queue_ = xQueueCreate(MAX_FRAMES_TO_SEND, sizeof(TxRecord));
  if (tx_queue_ == nullptr) {
    ESP_LOGE(TAG, "could not create the transmit queue");
    this->mark_failed();
    return;
  }
  BaseType_t created = (tx_core_ < 0)
      ? xTaskCreate(HDMICEC::tx_task_entry_, "hdmi_cec_tx", TX_TASK_STACK_WORDS, this,
                    TX_TASK_PRIORITY, &tx_task_)
      : xTaskCreatePinnedToCore(HDMICEC::tx_task_entry_, "hdmi_cec_tx", TX_TASK_STACK_WORDS, this,
                                TX_TASK_PRIORITY, &tx_task_, tx_core_);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "could not create the transmit task");
    this->mark_failed();
    return;
  }

  created = (tx_core_ < 0) ? xTaskCreate(HDMICEC::rx_task_entry_, "hdmi_cec_rx", RX_TASK_STACK_WORDS, this,
                                         RX_TASK_PRIORITY, &rx_task_)
                           : xTaskCreatePinnedToCore(HDMICEC::rx_task_entry_, "hdmi_cec_rx", RX_TASK_STACK_WORDS, this,
                                                     RX_TASK_PRIORITY, &rx_task_, tx_core_);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "could not create the receive task");
    this->mark_failed();
    return;
  }
#endif

  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  set_pin_input_high();
}

#ifdef HDMI_CEC_USE_FREERTOS
void HDMICEC::rx_task_entry_(void *param) { static_cast<HDMICEC *>(param)->rx_task_loop_(); }

void HDMICEC::rx_task_loop_() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (edge_overflow_.exchange(false)) {
      ESP_LOGW(TAG, "edge queue overflow, dropping the frame in flight");
      receiver_state_ = ReceiverState::Idle;
      recv_ack_queued_ = false;
      frame_receive_ = nullptr;
      reset_state_variables_(this);
    }

    while (true) {
      const uint16_t tail = edge_tail_.load(std::memory_order_relaxed);
      if (tail == edge_head_.load(std::memory_order_acquire)) {
        break;
      }
      const EdgeEvent event = edge_queue_[tail];
      edge_tail_.store((uint16_t) ((tail + 1) % EDGE_QUEUE_SIZE), std::memory_order_release);
      process_edge_(this, event.level, event.us);
    }
  }
}

void HDMICEC::tx_task_entry_(void *param) { static_cast<HDMICEC *>(param)->tx_task_loop_(); }

void HDMICEC::tx_task_loop_() {
  TxRecord request;
  while (true) {
    // The only blocking point. Everything below runs to completion before the next frame is
    // taken, so the bus has one writer and the retransmission state needs no locking.
    if (xQueueReceive(tx_queue_, &request, portMAX_DELAY) != pdTRUE || request.length < 1) {
      continue;
    }
    Frame frame;
    frame.assign(request.bytes, request.bytes + request.length);
    bool is_broadcast = frame.is_broadcast();

    ESP_LOGD(TAG, "[sending] %s", frame.to_string().c_str());
    SendResult result = send_with_retries_(frame, is_broadcast);
    publish_result_(frame, result);
  }
}
#endif

void HDMICEC::publish_result_(const Frame &frame, SendResult result) {
  TxResultRecord record;
  record.length = (uint8_t) std::min((size_t) Frame::MAX_LENGTH, frame.size());
  std::copy(frame.begin(), frame.begin() + record.length, record.bytes);
  record.result = result;

  LockGuard lock(tx_results_mutex_);
  if (tx_results_.size() >= (size_t) MAX_FRAMES_TO_SEND) {
    // The loop task is not draining. Dropping the oldest keeps the newest, which is the one
    // an automation is most likely still waiting on.
    tx_results_.erase(tx_results_.begin());
  }
  tx_results_.push_back(record);
}

void HDMICEC::process_send_results_() {
  std::vector<TxResultRecord> results;
  {
    LockGuard lock(tx_results_mutex_);
    if (tx_results_.empty()) {
      return;
    }
    results.swap(tx_results_);
  }

  for (const auto &record : results) {
    if (record.length < 1) {
      continue;
    }
    uint8_t src_addr = ((record.bytes[0] & 0xF0) >> 4);
    uint8_t dest_addr = (record.bytes[0] & 0x0F);
    uint8_t opcode = (record.length >= 2) ? record.bytes[1] : 0;
    std::vector<uint8_t> data(record.bytes + 1, record.bytes + record.length);
    bool success = (record.result == SendResult::Success);

    if (!success) {
      ESP_LOGI(TAG, "HDMICEC::send(): frame not sent: %s",
               ((record.result == SendResult::BusCollision) ? "Bus Collision" : "No Ack received"));
    }

    for (auto trigger : send_result_triggers_) {
      bool can_trigger = (
        (!trigger->source_.has_value()      || (trigger->source_ == src_addr)) &&
        (!trigger->destination_.has_value() || (trigger->destination_ == dest_addr)) &&
        (!trigger->opcode_.has_value()      || (trigger->opcode_ == opcode))
      );
      if (can_trigger) {
        trigger->trigger(src_addr, dest_addr, data, success);
      }
    }
  }
}

void HDMICEC::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC");
  LOG_PIN("  pin: ", pin_);
  ESP_LOGCONFIG(TAG, "  address: %x", address_);
  ESP_LOGCONFIG(TAG, "  promiscuous mode: %s", (promiscuous_mode_ ? "yes" : "no"));
  ESP_LOGCONFIG(TAG, "  monitor mode: %s", (monitor_mode_ ? "yes" : "no"));
#ifdef HDMI_CEC_USE_FREERTOS
  if (tx_core_ < 0) {
    ESP_LOGCONFIG(TAG, "  transmit task core: any");
  } else {
    ESP_LOGCONFIG(TAG, "  transmit task core: %d", tx_core_);
  }
#endif
}

void HDMICEC::loop() {
  // Before the received frames, so an automation that sends from on_message sees the result
  // of its previous send first.
  process_send_results_();

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

  Frame frame(source, destination, data_bytes);
  if (frame.size() > (size_t) Frame::MAX_LENGTH) {
    ESP_LOGW(TAG, "HDMICEC::send(): frame of %u bytes exceeds the CEC maximum", (unsigned) frame.size());
    return false;
  }

#ifdef HDMI_CEC_USE_FREERTOS
  TxRecord request;
  request.length = (uint8_t) frame.size();
  std::copy(frame.begin(), frame.end(), request.bytes);
  // Never waits: this runs on the loop task, and a full queue means the bus is already
  // backed up by a second of traffic. Failing here is better than stalling every automation
  // behind it.
  if (xQueueSend(tx_queue_, &request, 0) != pdTRUE) {
    ESP_LOGW(TAG, "HDMICEC::send(): transmit queue full, dropping %s", frame.to_string(true).c_str());
    return false;
  }
  return true;
#else
  // No FreeRTOS here, so the caller pays for the frame as before. The result still travels
  // through the same buffer, so the trigger fires from loop() on every platform.
  ESP_LOGD(TAG, "[sending] %s", frame.to_string().c_str());
  publish_result_(frame, send_with_retries_(frame, frame.is_broadcast()));
  return true;
#endif
}

SendResult HDMICEC::send_with_retries_(const Frame &frame, bool is_broadcast) {
  SendResult last_result = SendResult::NoAck;
  {
    // Bus 'Signal Free' time between transmissions, according to the HDMI-CEC standard, shall be a minimum of:
    //  - 7 bit periods between successive transmissions of same sender
    //  - 5 bit periods between transmissions of different senders
    //  - 3 bit periods for resend of a failed transmission attempt
    uint8_t free_bit_periods = (last_sent_us_.load() > last_falling_edge_us_) ? 7 : 5;

    // Total timeout: abort if we can't send within 2 seconds (prevents infinite blocking on busy bus)
    static const uint32_t SEND_TIMEOUT_US = 2000000;
    const uint32_t send_start_us = micros();

    for (size_t i = 0; i < MAX_ATTEMPTS; i++) {
      int32_t delay = 0;
      // Per-attempt timeout for bus-free wait: 200ms max per attempt
      const uint32_t attempt_start_us = micros();
      static const uint32_t ATTEMPT_TIMEOUT_US = 200000;

      while ((delay = free_bit_periods * TOTAL_BIT_US + std::max(last_sent_us_.load(), (uint32_t) last_falling_edge_us_) - micros()) > 0) {
        // Check total timeout
        if ((micros() - send_start_us) > SEND_TIMEOUT_US) {
          ESP_LOGW(TAG, "HDMICEC::send(): total timeout reached (2s), aborting");
          return SendResult::BusCollision;
        }
        // Check per-attempt timeout (bus constantly busy)
        if ((micros() - attempt_start_us) > ATTEMPT_TIMEOUT_US) {
          ESP_LOGW(TAG, "HDMICEC::send(): attempt %d bus-wait timeout (200ms), retrying", i + 1);
          break;
        }
        ESP_LOGV(TAG, "HDMICEC::send(): waiting %d usec for bus free period", delay);
        if (delay >= (int32_t) YIELD_INTERVAL_US) {
#ifdef HDMI_CEC_USE_FREERTOS
          // A bus-free period is 12000-16800 us, so the millisecond granularity of a tick
          // costs nothing here and the core is free for other work meanwhile. Bit timing
          // below stays on delay_microseconds_safe(); 2400 us is not schedulable.
          vTaskDelay(pdMS_TO_TICKS(YIELD_INTERVAL_US / 1000));
#else
          delay_microseconds_safe(YIELD_INTERVAL_US);
          yield();
#endif
        } else {
          delay_microseconds_safe(delay);
        }
        // Note: during this delay, the 'last_falling_edge_us_' might be incremented by 'gpio_intr_', requiring further wait
        free_bit_periods = 5;
      }

      // Skip frame send if we broke out due to per-attempt timeout
      if ((micros() - attempt_start_us) > ATTEMPT_TIMEOUT_US) {
        free_bit_periods = 3;
        yield();
        continue;
      }

      ESP_LOGV(TAG, "HDMICEC::send(): bus available, sending frame...");

      last_result = send_frame_(frame, is_broadcast);
      if (last_result == SendResult::Success) {
        ESP_LOGD(TAG, "frame sent and acknowledged");
        return last_result;
      }
      ESP_LOGV(TAG, "HDMICEC::send(): attempt %d failed: %s", i + 1,
               ((last_result == SendResult::BusCollision) ? "Bus Collision" : "No Ack received"));
      // attempt retransmission with smaller free time gap
      free_bit_periods = 3;
      yield();
    }
  }

  ESP_LOGE(TAG, "HDMICEC::send(): send failed after %d attempts", MAX_ATTEMPTS);
  return last_result;
}

SendResult HDMICEC::send_frame_(const Frame &frame, bool is_broadcast) {
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
  last_sent_us_.store(micros());
  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  return result;
}

bool HDMICEC::send_start_bit_() {
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

void HDMICEC::send_bit_(bool bit_value) {
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

bool HDMICEC::send_high_and_test_() {
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

  if (level == false) {
    self->last_falling_edge_us_ = now;
  }

#ifdef HDMI_CEC_USE_FREERTOS
  const uint16_t head = self->edge_head_.load(std::memory_order_relaxed);
  const uint16_t next = (uint16_t) ((head + 1) % EDGE_QUEUE_SIZE);
  if (next == self->edge_tail_.load(std::memory_order_acquire)) {
    self->edge_overflow_.store(true, std::memory_order_relaxed);
    return;
  }
  self->edge_queue_[head].us = now;
  self->edge_queue_[head].level = level;
  self->edge_head_.store(next, std::memory_order_release);

  BaseType_t higher_woken = pdFALSE;
  vTaskNotifyGiveFromISR(self->rx_task_, &higher_woken);
  if (higher_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
#else
  process_edge_(self, level, now);
#endif
}

void HDMI_CEC_RX_ATTR HDMICEC::drive_ack_(HDMICEC *self, uint32_t edge_us) {
#ifdef HDMI_CEC_USE_FREERTOS
  self->set_pin_output_low();
  const uint32_t elapsed = micros() - edge_us;
  if (elapsed < LOW_BIT_US) {
    delay_microseconds_safe(LOW_BIT_US - elapsed);
  }
  self->set_pin_input_high();
#else
  InterruptLock interrupt_lock;
  self->set_pin_output_low();
  delay_microseconds_safe(LOW_BIT_US);
  self->set_pin_input_high();
#endif
}

void HDMI_CEC_RX_ATTR HDMICEC::process_edge_(HDMICEC *self, bool level, uint32_t now) {
  // on falling edge, store current time as the start of the low pulse
  if (level == false) {
    self->rx_last_falling_us_ = now;

    if (self->recv_ack_queued_ && !self->monitor_mode_) {
      self->recv_ack_queued_ = false;
      drive_ack_(self, now);
    }
    return;
  }
  // otherwise, it's a rising edge, so it's time to process the pulse length

  auto pulse_duration = (now - self->rx_last_falling_us_);

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

void HDMI_CEC_RX_ATTR HDMICEC::reset_state_variables_(HDMICEC *self) {
  self->recv_bit_counter_ = 0;
  self->recv_byte_buffer_ = 0x0;
}

}
}

#pragma once

#include <vector>
#include <queue>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace hdmi_cec {

class Frame : public std::vector<uint8_t> {
 public:
  Frame() = default;
  Frame(uint8_t initiator_addr, uint8_t target_addr, const std::vector<uint8_t> &payload);
  uint8_t initiator_addr() const { return (this->at(0) >> 4) & 0xf; }
  uint8_t destination_addr() const { return this->at(0) & 0xf; }
  uint8_t opcode() const { return (this->size() >= 2) ? this->at(1) : 0; }
  bool is_broadcast() const { return this->destination_addr() == 0xf; }
  std::string to_string(uint8_t my_address) const;
  constexpr static int MAX_LENGTH = 16;  // from HDMI CEC standard 1.4
};

enum class ReceiverState : uint8_t {
  Idle = 0,
  ReceivingByte = 2,
  WaitingForEOM = 3,
  WaitingForAck = 4,
  WaitingForEOMAck = 5,
};

enum class SendResult : uint8_t {
  Success = 0,
  BusCollision = 1,
  NoAck = 2,
};

class MessageTrigger;

class HDMICEC : public Component {
public:
  void set_pin(InternalGPIOPin *pin) { pin_ = pin; }
  void set_address(uint8_t address) { address_ = address; }
  uint8_t address() { return address_; }
  void set_physical_address(uint16_t physical_address) { physical_address_ = physical_address; }
  void set_promiscuous_mode(bool promiscuous_mode) { promiscuous_mode_ = promiscuous_mode; }
  void set_monitor_mode(bool monitor_mode) { monitor_mode_ = monitor_mode; }
  void set_osd_name_bytes(const std::vector<uint8_t> &osd_name_bytes) { osd_name_bytes_ = osd_name_bytes; }
  void add_message_trigger(MessageTrigger *trigger) { message_triggers_.push_back(trigger); }

  bool send(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data_bytes);

  // Component overrides
  float get_setup_priority() { return esphome::setup_priority::HARDWARE; }
  void setup() override;
  void dump_config() override;
  void loop() override;

protected:
  static void gpio_intr_(HDMICEC *self);
  static void reset_state_variables_(HDMICEC *self);
  void try_builtin_handler_(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data);
  SendResult send_frame_(const Frame &frame, bool is_broadcast);
  bool send_start_bit_();
  void send_bit_(bool bit_value);
  bool send_high_and_test_();
  void set_pin_input_high();
  void set_pin_output_low();

  InternalGPIOPin *pin_;
  ISRInternalGPIOPin isr_pin_;
  uint8_t address_;
  uint16_t physical_address_;
  bool promiscuous_mode_;
  bool monitor_mode_;
  std::vector<uint8_t> osd_name_bytes_;
  std::vector<MessageTrigger*> message_triggers_;

  uint32_t last_falling_edge_us_; // timepoint in received message
  uint32_t last_sent_us_;         // timepoint on end of sent message
  ReceiverState receiver_state_;
  uint8_t recv_bit_counter_;
  uint8_t recv_byte_buffer_;
  Frame recv_frame_buffer_;
  std::queue<Frame> recv_queue_;
  bool recv_ack_queued_;
  Mutex send_mutex_;
};

class MessageTrigger : public Trigger<uint8_t, uint8_t, std::vector<uint8_t>> {
  friend class HDMICEC;

public:
  explicit MessageTrigger(HDMICEC *parent) { parent->add_message_trigger(this); };
  void set_source(uint8_t source) { source_ = source; };
  void set_destination(uint8_t destination) { destination_ = destination; };
  void set_opcode(uint8_t opcode) { opcode_ = opcode; };
  void set_data(const std::vector<uint8_t> &data) { data_ = data; };

protected:
  optional<uint8_t> source_;
  optional<uint8_t> destination_;
  optional<uint8_t> opcode_;
  optional<std::vector<uint8_t>> data_;
};

template<typename... Ts> class SendAction : public Action<Ts...> {
public:
  SendAction(HDMICEC *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, source)
  TEMPLATABLE_VALUE(uint8_t, destination)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

  void play(Ts... x) override {
    auto source_address = source_.has_value() ? source_.value(x...) : parent_->address();
    auto destination_address = destination_.value(x...);
    auto data = data_.value(x...);
    parent_->send(source_address, destination_address, data);
  }

protected:
  HDMICEC *parent_;
};

}
}

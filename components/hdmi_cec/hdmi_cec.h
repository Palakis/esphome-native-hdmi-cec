#pragma once

#include <array>
#include <vector>
#include <atomic>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace hdmi_cec {

using Frame = std::vector<uint8_t>;
constexpr static int MAX_FRAME_SIZE = 16;  // according to hdmi cec standard
std::string bytes_to_string(const Frame *bytes);

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

/*
* The FrameRingBuffer is a container for Frames to queue data in a consumer-producer
* application. The use of std::Atomics allows safe multi-thread operation when used with
* a single producer and single consumer thread, where each Atomic index is updated
* by one thread only.
* After initialization, it operates without dynamic memmory allocation.
* This allows the gpio isr to safely and efficiently pick-up and pass Frames.
* Due to its fixed memory size, it might return NULL pointers in case the buffer is full or empty.
*/
template <unsigned int SIZE>
class FrameRingBuffer {
  public:
  FrameRingBuffer()
  : front_inx_{0}
  , back_inx_{0}
  , store_{} {
    for (auto& t : store_) {
      t = new Frame;
      t->reserve(MAX_FRAME_SIZE);
    }
  }
  ~FrameRingBuffer() {
    for (auto& t : store_) {
      delete t;
    }
  }
  // 'front' is used to access data, use that, and recycle its memory space for later use.
  Frame* front() const { return is_empty() ? nullptr : store_[front_inx_]; }
  void push_front() { cyclic_incr(front_inx_); }
  // 'back' is used to fetch a free Frame, fill with data, and queue for later pick-up
  Frame* back() const { return is_full() ? nullptr : (store_[back_inx_]->clear(), store_[back_inx_]); }
  void push_back() { cyclic_incr(back_inx_); }
  bool is_empty() const {return count() == 0;}
  bool is_full() const {return count() == SIZE;}  // using safe wrap-around of unsignd int
  void reset() {front_inx_ = 0; back_inx_ = 0;}

  protected:
  using Index = std::atomic<unsigned int>;
  // this simple increment scheme is sufficiently 'atomic' if the front and back are each used by
  // one thread only. (So, at most one reader thread and one writer thread in the application.)
  int count() const {int n = (int)(back_inx_ - front_inx_); if (n < 0) n += SIZE + 1; return n;}
  void cyclic_incr(Index &inx) { inx = (inx == SIZE) ? 0 : (inx + 1); }
  Index front_inx_;  // ranging 0 .. SIZE
  Index back_inx_;   // ranging 0 .. SIZE
  // if front_inx_ == back_inx_ the store is empty, so it can hold at most SIZE elements
  std::array<Frame*, SIZE + 1> store_;
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
  SendResult send_frame_(const std::vector<uint8_t> &frame, bool is_broadcast);
  bool send_start_bit_();
  void send_bit_(bool bit_value);
  bool send_high_and_test_();
  void set_pin_input_high();
  void set_pin_output_low();

  constexpr static int MAX_FRAME_LEN = 16;
  constexpr static int MAX_FRAMES_QUEUED = 4;
  InternalGPIOPin *pin_;
  ISRInternalGPIOPin isr_pin_;
  uint8_t address_;
  uint16_t physical_address_;
  bool promiscuous_mode_;
  bool monitor_mode_;
  std::vector<uint8_t> osd_name_bytes_;
  std::vector<MessageTrigger*> message_triggers_;

  bool last_level_ = true;            // cec line level on last isr call
  uint32_t last_falling_edge_us_ = 0; // timepoint in received message
  uint32_t last_sent_us_ = 0;         // timepoint on end of sent message
  ReceiverState receiver_state_;
  uint8_t recv_bit_counter_ = 0;
  uint8_t recv_byte_buffer_ = 0;
  Frame *frame_receive_ = nullptr;
  FrameRingBuffer<4> frames_queue_;
  bool recv_ack_queued_ = false;
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

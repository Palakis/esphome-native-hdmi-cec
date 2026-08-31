#pragma once

#include <array>
#include <vector>
#include <atomic>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

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
  std::string to_string(bool skip_decode = 0) const;
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

// A frame waiting to go out, and one that has been out.
//
// Both are flat and copyable so they can travel through a FreeRTOS queue by value. A Frame
// is a std::vector and would have to be passed by pointer, which would put its lifetime in
// the hands of two tasks; the sixteen bytes a CEC frame can hold are cheaper to copy than
// that ownership question is to answer.
struct TxRecord {
  uint8_t length{0};
  uint8_t bytes[Frame::MAX_LENGTH]{};
};

struct TxResultRecord : public TxRecord {
  SendResult result{SendResult::Success};
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
      t->reserve(Frame::MAX_LENGTH);
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
class SendResultTrigger;

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
  void add_send_result_trigger(SendResultTrigger *trigger) { send_result_triggers_.push_back(trigger); }
  // Core the transmit task runs on, or -1 for no affinity. Sending holds a core for the
  // duration of the frame, so a build that also does real-time work wants the two apart.
  void set_tx_core(int8_t tx_core) { tx_core_ = tx_core; }

  // Hands a frame to the transmitter. Returns whether it was accepted, not whether it
  // reached the bus -- that answer arrives later, through on_send_result. Sending a CEC
  // frame takes 24 ms per byte plus up to 200 ms of waiting for the bus per attempt, five
  // attempts deep, and none of that may happen on the loop task.
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
  // Bus-free wait, arbitration and retransmission. Blocks for as long as the bus makes it;
  // only ever called from the transmit task on ESP32, and from send() elsewhere.
  SendResult send_with_retries_(const Frame &frame, bool is_broadcast);
  void publish_result_(const Frame &frame, SendResult result);
  void process_send_results_();
  SendResult send_frame_(const Frame &frame, bool is_broadcast);
  bool send_start_bit_();
  void send_bit_(bool bit_value);
  bool send_high_and_test_();
  void set_pin_input_high();
  void set_pin_output_low();

#ifdef USE_ESP32
  static void tx_task_entry_(void *param);
  void tx_task_loop_();

  TaskHandle_t tx_task_{nullptr};
  QueueHandle_t tx_queue_{nullptr};
#endif
  // Results are collected here by whoever sent the frame and drained by loop(), so the
  // triggers run on the loop task like every other automation in the component.
  std::vector<TxResultRecord> tx_results_;
  Mutex tx_results_mutex_;
  int8_t tx_core_{-1};

  constexpr static int MAX_FRAMES_QUEUED = 4;
  constexpr static int MAX_FRAMES_TO_SEND = 8;
  constexpr static int TX_TASK_STACK_WORDS = 3072;
  // Above the loop task, so a queued frame is not left waiting behind ordinary work, and
  // well below the timer and Wi-Fi tasks.
  constexpr static int TX_TASK_PRIORITY = 10;
  InternalGPIOPin *pin_;
  ISRInternalGPIOPin isr_pin_;
  uint8_t address_;
  uint16_t physical_address_;
  bool promiscuous_mode_;
  bool monitor_mode_;
  std::vector<uint8_t> osd_name_bytes_;
  std::vector<MessageTrigger*> message_triggers_;

  bool last_level_ = true;            // cec line level on last isr call
  volatile uint32_t last_falling_edge_us_ = 0; // timepoint in received message (volatile: written by ISR, read by send())
  // Written by the transmit task, read by it and by the bus-free calculation. Atomic
  // because those are no longer the same task.
  std::atomic<uint32_t> last_sent_us_{0};  // timepoint on end of sent message
  ReceiverState receiver_state_;
  uint8_t recv_bit_counter_ = 0;
  uint8_t recv_byte_buffer_ = 0;
  Frame *frame_receive_ = nullptr;
  FrameRingBuffer<MAX_FRAMES_QUEUED> frames_queue_;
  bool recv_ack_queued_ = false;
  std::vector<SendResultTrigger*> send_result_triggers_;
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

// Fires once per frame the transmitter is done with, successful or not. The filters match
// the frame that was sent, so an automation can wait for one particular message to be
// acknowledged before acting on it.
class SendResultTrigger : public Trigger<uint8_t, uint8_t, std::vector<uint8_t>, bool> {
  friend class HDMICEC;

public:
  explicit SendResultTrigger(HDMICEC *parent) { parent->add_send_result_trigger(this); };
  void set_source(uint8_t source) { source_ = source; };
  void set_destination(uint8_t destination) { destination_ = destination; };
  void set_opcode(uint8_t opcode) { opcode_ = opcode; };

protected:
  optional<uint8_t> source_;
  optional<uint8_t> destination_;
  optional<uint8_t> opcode_;
};

template<typename... Ts> class SendAction : public Action<Ts...> {
public:
  SendAction(HDMICEC *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, source)
  TEMPLATABLE_VALUE(uint8_t, destination)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

  void play(const Ts&... x) override {
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

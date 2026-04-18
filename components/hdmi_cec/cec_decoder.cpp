#include "cec_decoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hdmi_cec.h"

namespace esphome {
namespace hdmi_cec {

static const std::array<const char *, 0x77> UI_COMMANDS = {
    "Select",                        // 0x00
    "Up",                            // 0x01
    "Down",                          // 0x02
    "Left",                          // 0x03
    "Right",                         // 0x04
    "Right-Up",                      // 0x05
    "Right-Down",                    // 0x06
    "Left-Up",                       // 0x07
    "Left-Down",                     // 0x08
    "Root Menu",                     // 0x09
    "Setup Menu",                    // 0x0A
    "Contents Menu",                 // 0x0B
    "Favorite Menu",                 // 0x0C
    "Exit",                          // 0x0D
    "Reserved",                      // 0x0E
    "Reserved",                      // 0x0F
    "Media Top Menu",                // 0x10
    "Media Context-sensitive Menu",  // 0x11
    "Reserved",                      // 0x12
    "Reserved",                      // 0x13
    "Reserved",                      // 0x14
    "Reserved",                      // 0x15
    "Reserved",                      // 0x16
    "Reserved",                      // 0x17
    "Reserved",                      // 0x18
    "Reserved",                      // 0x19
    "Reserved",                      // 0x1A
    "Reserved",                      // 0x1B
    "Reserved",                      // 0x1C
    "Number Entry Mode",             // 0x1D
    "11",                            // 0x1E
    "12",                            // 0x1F
    "0",                             // 0x20
    "1",                             // 0x21
    "2",                             // 0x22
    "3",                             // 0x23
    "4",                             // 0x24
    "5",                             // 0x25
    "6",                             // 0x26
    "7",                             // 0x27
    "8",                             // 0x28
    "9",                             // 0x29
    "Dot",                           // 0x2A
    "Enter",                         // 0x2B
    "Clear",                         // 0x2C
    "Reserved",                      // 0x2D
    "Reserved",                      // 0x2E
    "Next Favorite",                 // 0x2F
    "Channel Up",                    // 0x30
    "Channel Down",                  // 0x31
    "Previous Channel",              // 0x32
    "Sound Select",                  // 0x33
    "Input Select",                  // 0x34
    "Display Information",           // 0x35
    "Help",                          // 0x36
    "Page Up",                       // 0x37
    "Page Down",                     // 0x38
    "Reserved",                      // 0x39
    "Reserved",                      // 0x3A
    "Reserved",                      // 0x3B
    "Reserved",                      // 0x3C
    "Reserved",                      // 0x3D
    "Reserved",                      // 0x3E
    "Reserved",                      // 0x3F
    "Power",                         // 0x40
    "Volume Up",                     // 0x41
    "Volume Down",                   // 0x42
    "Mute",                          // 0x43
    "Play",                          // 0x44
    "Stop",                          // 0x45
    "Pause",                         // 0x46
    "Record",                        // 0x47
    "Rewind",                        // 0x48
    "Fast forward",                  // 0x49
    "Eject",                         // 0x4A
    "Forward",                       // 0x4B
    "Backward",                      // 0x4C
    "Stop-Record",                   // 0x4D
    "Pause-Record",                  // 0x4E
    "Reserved",                      // 0x4F
    "Angle",                         // 0x50
    "Sub picture",                   // 0x51
    "Video on Demand",               // 0x52
    "Electronic Program Guide",      // 0x53
    "Timer Programming",             // 0x54
    "Initial Configuration",         // 0x55
    "Select Broadcast Type",         // 0x56
    "Select Sound Presentation",     // 0x57
    "Reserved",                      // 0x58
    "Reserved",                      // 0x59
    "Reserved",                      // 0x5A
    "Reserved",                      // 0x5B
    "Reserved",                      // 0x5C
    "Reserved",                      // 0x5D
    "Reserved",                      // 0x5E
    "Reserved",                      // 0x5F
    "Play Function",                 // 0x60
    "Pause-Play Function",           // 0x61
    "Record Function",               // 0x62
    "Pause-Record Function",         // 0x63
    "Stop Function",                 // 0x64
    "Mute Function",                 // 0x65
    "Restore Volume Function",       // 0x66
    "Tune Function",                 // 0x67
    "Select Media Function",         // 0x68
    "Select A/V Input Function",     // 0x69
    "Select Audio Input Function",   // 0x6A
    "Power Toggle Function",         // 0x6B
    "Power Off Function",            // 0x6C
    "Power On Function",             // 0x6D
    "Reserved",                      // 0x6E
    "Reserved",                      // 0x6F
    "Reserved",                      // 0x70
    "F1 (Blue)",                     // 0x71
    "F2 (Red)",                      // 0x72
    "F3 (Green)",                    // 0x73
    "F4 (Yellow)",                   // 0x74
    "F5",                            // 0x75
    "Data",                          // 0x76
};

// See 'short audio descriptor' in https://en.wikipedia.org/wiki/Extended_Display_Identification_Data
static const std::array<const char *, 0x11> AUDIO_FORMATS = {
    "reserved",          // 0x00
    "LPCM",              // 0x01
    "AC3",               // 0x02
    "MPEG-1",            // 0x03
    "MP3",               // 0x04
    "MPEG-2",            // 0x05
    "AAC",               // 0x06
    "DTS",               // 0x07
    "ATRAC",             // 0x08
    "DSD",               // 0x09
    "DD+",               // 0x0A
    "DTS-HD",            // 0x0B
    "MAT/Dolby TrueHD",  // 0x0C
    "DST Audio",         // 0x0D
    "WMA Pro",           // 0x0E
    "Extension?",        // 0x0F
};
static const std::array<const char *, 8> AUDIO_SAMPLERATES = {
    "32",        // 0 (bit 0)
    "44.1",      // 1 (bit 1)
    "48",        // 2 (bit 2)
    "88",        // 3 (bit 3)
    "96",        // 4 (bit 4)
    "176",       // 5 (bit 5)
    "192",       // 6 (bit 6)
    "Reserved",  // 7 (bit 7)
};

template<uint32_t OPERANDS> bool Decoder::do_operand() {
  if (OPERANDS <= 0xFF) {
    // generic function called for single operand of unkown type and length
    return append_operand(".");
  } else {
    return do_operand<OPERANDS & 0xFF>() && do_operand<(OPERANDS >> 8u)>();
  }
}

/**
 * List of specialised operand decode functions, one function per operand type.
 * (Not fully complete, might be extended later)
 */
template<> bool Decoder::do_operand<Decoder::None>() { return append_operand(""); }

template<> bool Decoder::do_operand<Decoder::AbortReason>() {
  const static std::array<const char *, 7> names = {
      "Unrecognized opcode", "Not in correct mode to respond", "Cannot provide source", "Invalid operand", "Refused",
      "Unable to determine"};
  return append_operand<names.size()>(names);
}

template<> bool Decoder::do_operand<Decoder::AudioFormat>() {
  // this type of operand comes in a sequence, until exhausted
  bool ok = true;
  while (ok && (offset_ < frame_.size())) {
    ok = append_operand<AUDIO_FORMATS.size()>(AUDIO_FORMATS);
  }
  return ok;
}

template<> bool Decoder::do_operand<Decoder::AudioStatus>() {
  char line[20];
  if (offset_ < frame_.size()) {
    uint8_t field = frame_[offset_];
    std::sprintf(line, "Mute=%d,Vol=%02X", (field >> 7), (field & 0x7f));
    return append_operand(line);
  } else {
    return append_operand("?");
  }
}

const char *Decoder::find_device_type_name(uint8_t type) {
  // clang-format off
  static const char *names[] = {
      "TV",               // 0x00
      "Recording Device", // 0x01
      "Reserved",         // 0x02
      "Tuner",            // 0x03
      "Playback Device",  // 0x04
      "Audio System",     // 0x05
      "Pure CEC Switch",  // 0x06
      "Video Processor",  // 0x07
  };
  // clang-format on
  if (type < sizeof(names) / sizeof(names[0]))
    return names[type];
  return nullptr;
}

template<> bool Decoder::do_operand<Decoder::DeviceType>() {
  const char *name = find_device_type_name(frame_[offset_]);
  return append_operand(name ? name : "?");
}

template<> bool Decoder::do_operand<Decoder::DisplayControl>() {
  const static std::array<const char *, 9> names = {"Default Time", "Until cleared", "Clear previous", "Reserved"};
  return append_operand<names.size()>(names);
}

template<> bool Decoder::do_operand<Decoder::FeatureOpcode>() {
  if (offset_ >= frame_.size()) {
    return false;
  }
  uint8_t opcode = frame_[offset_];
  return append_operand(find_opcode_name(opcode));
}

template<> bool Decoder::do_operand<Decoder::OsdString>() {
  char line[20];  // frame size() is at most 16
  // copy with typecast and append '\0' char to terminate string
  std::snprintf(line, frame_.size() - offset_ + 1, "%s", reinterpret_cast<const char *>(frame_.data() + offset_));
  offset_ = frame_.size();
  return append_operand(line);
}

template<> bool Decoder::do_operand<Decoder::PhysicalAddress>() {
  // Exception: if this is an operand of <System Audio Mode Request> 0x70, then this operand is
  // merely optional, and its absence means 'Off'
  if (frame_.at(1) == 0x70 && offset_ >= frame_.size()) {
    return append_operand("Off");
  }
  if (offset_ + 1 >= frame_.size()) {
    return append_operand("?", 2);
  }
  char line[12];
  std::sprintf(line, "%1x.%1x.%1x.%1x", (frame_[offset_] >> 4) & 0xF, frame_[offset_] & 0xF,
               (frame_[offset_ + 1] >> 4) & 0xF, frame_[offset_ + 1] & 0xF);
  return append_operand(line, 2);
}

const char *Decoder::find_power_status_name(uint8_t status) {
  static const char *names[] = {
      "On",             // 0x00
      "Standby",        // 0x01
      "Standby to On",  // 0x02
      "On to Standby",  // 0x03
  };
  if (status < sizeof(names) / sizeof(names[0]))
    return names[status];
  return nullptr;
}

template<> bool Decoder::do_operand<Decoder::PowerStatus>() {
  const char *name = find_power_status_name(frame_[offset_]);
  return append_operand(name ? name : "?");
}

template<> bool Decoder::do_operand<Decoder::ShortAudioDescriptor>() {
  // the frame can have a sequence of these operands, count is not fixed;
  // each such operand takes 3 bytes in the frame
  std::array<char, 100> line;
  uint32_t pos = 0;
  bool ok = true;
  while (ok && (offset_ + 2 < frame_.size())) {
    const uint8_t *descriptor = frame_.data() + offset_;
    uint8_t format = (descriptor[0] >> 3) & 0x0F;
    pos = std::sprintf(&line[0], "%s", AUDIO_FORMATS[format]);
    pos += std::sprintf(&line[pos], ",num_channels=%d", (descriptor[0] & 0x07));
    uint8_t rates = descriptor[1];
    for (int bit = 0; rates; bit++, rates >>= 1) {
      if (rates & 0x1) {
        // show support of various audio sample rates
        pos += std::sprintf(&line[pos], ",%skHz", AUDIO_SAMPLERATES[bit]);
      }
    }
    if (format == 1) {
      // for LPCM format
      uint8_t widths = descriptor[2] & 0x7;
      for (int i = 0; widths; i++, widths >>= 1) {
        if (widths & 0x1) {
          // show support of audio samble bit widths of 16, 20, and/or 24
          pos += std::sprintf(&line[pos], ",%dbits", (16 + 4 * i));
        }
      }
    }
    ok = append_operand(&line[0], 3);
  }
  // TODO: Further descriptor 'extensions' not yet decoded
  return ok;
}

template<> bool Decoder::do_operand<Decoder::SystemAudioStatus>() {
  const static std::array<const char *, 3> names = {"Off", "On"};
  return append_operand<names.size()>(names);
}

template<> bool Decoder::do_operand<Decoder::UICommand>() {
  uint8_t command = frame_[offset_];
  bool ok = append_operand<UI_COMMANDS.size()>(UI_COMMANDS);
  if (!ok) {
    return false;
  }
  // out of the 100+ UI commands, a few exceptional UI commands have appended an extra parameter:
  switch (command) {
    case 0x56:
      return do_operand<UIBroadcastType>();
    case 0x57:
      return do_operand<UISoundPresentationControl>();
    case 0x60:
      return do_operand<PlayMode>();
    case 0x67:
      return do_operand<ChannelIdentifier>();
    case 0x68:
      return do_operand<UIFunctionMedia>();
    case 0x69:
      return do_operand<UIFunctionSelectAVInput>();
    case 0x6A:
      return do_operand<UIFunctionSelectAudioInput>();
    default:
      return ok;
  }
}

template<> bool Decoder::do_operand<Decoder::VendorId>() {
  if (offset_ + 2 >= frame_.size()) {
    return append_operand("?", 3);
  }
  uint32_t id = (uint32_t) (frame_[offset_]) << 16 | (uint32_t) (frame_[offset_ + 1]) << 8 | frame_[offset_ + 2];
  const char *name = find_vendor_name(id);
  if (name == nullptr) {
    // if the hdmi-cec vendor id is not in our list, the id value itself is printed.
    char line[12];
    sprintf(line, "ID=%06x", id);
    return append_operand(line, 3);
  }
  return append_operand(name, 3);
}

const char *Decoder::find_cec_version_name(uint8_t version) {
  static const char *names[] = {
      "1.1",   // 0x00
      "1.2",   // 0x01
      "1.2a",  // 0x02
      "1.3",   // 0x03
      "1.3a",  // 0x04
      "1.4",   // 0x05
      "2.0",   // 0x06
      "2.x",   // 0x07
      "2.x",   // 0x08
  };
  if (version < sizeof(names) / sizeof(names[0]))
    return names[version];
  return nullptr;
}

template<> bool Decoder::do_operand<Decoder::CecVersion>() {
  const char *name = find_cec_version_name(frame_[offset_]);
  return append_operand(name ? name : "?");
}

const Decoder::FrameType *Decoder::find_opcode(uint8_t opcode) {
  struct Entry {
    uint8_t opcode;
    FrameType type;
  };
  // Sorted by opcode for binary search
  static constexpr Entry table[] = {
      {0x00, {"Feature Abort", &Decoder::do_operand<Two(FeatureOpcode, AbortReason)>}},
      {0x04, {"Image View On", &Decoder::do_operand<None>}},
      {0x05, {"Tuner Step Increment", &Decoder::do_operand<None>}},
      {0x06, {"Tuner Step Decrement", &Decoder::do_operand<None>}},
      {0x07, {"Tuner Device Status", &Decoder::do_operand<TunerDeviceInfo>}},
      {0x08, {"Give Tuner Device Status", &Decoder::do_operand<StatusRequest>}},
      {0x09, {"Record On", &Decoder::do_operand<RecordSource>}},
      {0x0A, {"Record Status", &Decoder::do_operand<RecordStatusInfo>}},
      {0x0B, {"Record Off", &Decoder::do_operand<None>}},
      {0x0D, {"Text View On", &Decoder::do_operand<None>}},
      {0x0F, {"Record TV Screen", &Decoder::do_operand<None>}},
      {0x1A, {"Give Deck Status", &Decoder::do_operand<StatusRequest>}},
      {0x1B, {"Deck Status", &Decoder::do_operand<DeckInfo>}},
      {0x32, {"Set Menu Language", &Decoder::do_operand<Language>}},
      {0x33, {"Clear Analogue Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0x34, {"Set Analogue Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0x35, {"Timer Status", &Decoder::do_operand<TimerStatusData>}},
      {0x36, {"Standby", &Decoder::do_operand<None>}},
      {0x41, {"Play", &Decoder::do_operand<PlayMode>}},
      {0x42, {"Deck Control", &Decoder::do_operand<DeckControlMode>}},
      {0x43, {"Timer Cleared Status", &Decoder::do_operand<TimerClearedStatusData>}},
      {0x44, {"User Control Pressed", &Decoder::do_operand<UICommand>}},
      {0x45, {"User Control Released", &Decoder::do_operand<None>}},
      {0x46, {"Give OSD Name", &Decoder::do_operand<None>}},
      {0x47, {"Set OSD Name", &Decoder::do_operand<OsdName>}},
      {0x64, {"Set OSD String", &Decoder::do_operand<Two(DisplayControl, OsdString)>}},
      {0x67, {"Set Timer Program Title", &Decoder::do_operand<ProgramTitleString>}},
      {0x70, {"System Audio Mode Request", &Decoder::do_operand<PhysicalAddress>}},
      {0x71, {"Give Audio Status", &Decoder::do_operand<None>}},
      {0x72, {"Set System Audio Mode", &Decoder::do_operand<SystemAudioStatus>}},
      {0x7A, {"Report Audio Status", &Decoder::do_operand<AudioStatus>}},
      {0x7D, {"Give System Audio Mode Status", &Decoder::do_operand<None>}},
      {0x7E, {"System Audio Mode Status", &Decoder::do_operand<SystemAudioStatus>}},
      {0x80, {"Routing Change", &Decoder::do_operand<Two(PhysicalAddress, PhysicalAddress)>}},
      {0x81, {"Routing Information", &Decoder::do_operand<PhysicalAddress>}},
      {0x82, {"Active Source", &Decoder::do_operand<PhysicalAddress>}},
      {0x83, {"Give Physical Address", &Decoder::do_operand<None>}},
      {0x84, {"Report Physical Address", &Decoder::do_operand<Two(PhysicalAddress, DeviceType)>}},
      {0x85, {"Request Active Source", &Decoder::do_operand<None>}},
      {0x86, {"Set Stream Path", &Decoder::do_operand<PhysicalAddress>}},
      {0x87, {"Device Vendor ID", &Decoder::do_operand<VendorId>}},
      {0x89, {"Vendor Command", &Decoder::do_operand<VendorSpecificData>}},
      {0x8A, {"Vendor Remote Button Down", &Decoder::do_operand<VendorSpecificRCCode>}},
      {0x8B, {"Vendor Remote Button Up", &Decoder::do_operand<None>}},
      {0x8C, {"Give Device Vendor ID", &Decoder::do_operand<None>}},
      {0x8D, {"Menu Request", &Decoder::do_operand<MenuRequestType>}},
      {0x8E, {"Menu Status", &Decoder::do_operand<MenuState>}},
      {0x8F, {"Give Device Power Status", &Decoder::do_operand<None>}},
      {0x90, {"Report Power Status", &Decoder::do_operand<PowerStatus>}},
      {0x91, {"Get Menu Language", &Decoder::do_operand<None>}},
      {0x92,
       {"Select Analogue Service", &Decoder::do_operand<Three(AnalogBroadcastType, AnalogFrequency, BroadcastSystem)>}},
      {0x93, {"Select Digital Service", &Decoder::do_operand<DigitalServiceIdentification>}},
      {0x97, {"Set Digital Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0x99, {"Clear Digital Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0x9A, {"Set Audio Rate", &Decoder::do_operand<AudioRate>}},
      {0x9D, {"Inactive Source", &Decoder::do_operand<PhysicalAddress>}},
      {0x9E, {"CEC Version", &Decoder::do_operand<CecVersion>}},
      {0x9F, {"Get CEC Version", &Decoder::do_operand<None>}},
      {0xA0, {"Vendor Command With ID", &Decoder::do_operand<Two(VendorId, VendorSpecificData)>}},
      {0xA1, {"Clear External Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0xA2, {"Set External Timer", &Decoder::do_operand<Two(StartDateTime, Duration)>}},
      {0xA3, {"Report Short Audio Descriptor", &Decoder::do_operand<ShortAudioDescriptor>}},
      {0xA4, {"Request Short Audio Descriptor", &Decoder::do_operand<AudioFormat>}},
      {0xC0, {"Initiate ARC", &Decoder::do_operand<None>}},
      {0xC1, {"Report ARC Initiated", &Decoder::do_operand<None>}},
      {0xC2, {"Report ARC Terminated", &Decoder::do_operand<None>}},
      {0xC3, {"Request ARC Initiation", &Decoder::do_operand<None>}},
      {0xC4, {"Request ARC Termination", &Decoder::do_operand<None>}},
      {0xC5, {"Terminate ARC", &Decoder::do_operand<None>}},
      {0xF8, {"CDC Message", &Decoder::do_operand<None>}},
      {0xFF, {"Abort", &Decoder::do_operand<None>}},
  };
  static_assert(
      []() constexpr {
        for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++)
          if (table[i].opcode <= table[i - 1].opcode)
            return false;
        return true;
      }(),
      "opcode table must be sorted");

  auto *it = std::lower_bound(std::begin(table), std::end(table), opcode,
                              [](const Entry &e, uint8_t val) { return e.opcode < val; });
  if (it != std::end(table) && it->opcode == opcode)
    return &it->type;
  return nullptr;
}

const char *Decoder::find_vendor_name(uint32_t id) {
  struct Entry {
    uint32_t id;
    const char *name;
  };
  // Sorted by vendor ID for binary search
  static constexpr Entry table[] = {
      {0x000039, "Toshiba"},       {0x0000F0, "Samsung"},       {0x00044B, "NVIDIA"}, {0x0005CD, "Denon"},
      {0x000678, "Marantz"},       {0x000982, "Loewe"},         {0x0009B0, "Onkyo"},  {0x000CB8, "Medion"},
      {0x000CE7, "Toshiba"},       {0x000D4B, "Roku"},          {0x0010FA, "Apple"},  {0x001582, "Pulse Eight"},
      {0x001950, "Harman Kardon"}, {0x001A11, "Google"},        {0x0020C7, "Akai"},   {0x002467, "AOC"},
      {0x008045, "Panasonic"},     {0x00903E, "Philips"},       {0x009053, "Daewoo"}, {0x00A0DE, "Yamaha"},
      {0x00D0D5, "Grundig"},       {0x00E036, "Pioneer"},       {0x00E091, "LG"},     {0x08001F, "Sharp"},
      {0x080046, "Sony"},          {0x18C086, "Broadcom"},      {0x534850, "Sharp"},  {0x6B746D, "Vizio"},
      {0x8065E9, "BenQ"},          {0x9C645E, "Harman Kardon"},
  };
  static_assert(
      []() constexpr {
        for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++)
          if (table[i].id <= table[i - 1].id)
            return false;
        return true;
      }(),
      "vendor table must be sorted");

  auto *it =
      std::lower_bound(std::begin(table), std::end(table), id, [](const Entry &e, uint32_t val) { return e.id < val; });
  if (it != std::end(table) && it->id == id)
    return it->name;
  return nullptr;
}

std::string Decoder::address_decode() const {
  const static std::array<const char *, 16> names = {
      "TV",             // 0x0
      "RecordingDev1",  // 0x1
      "RecordingDev2",  // 0x2
      "Tuner1",         // 0x3
      "PlaybackDev1",   // 0x4
      "AudioSystem",    // 0x5
      "Tuner2",         // 0x6
      "Tuner3",         // 0x7
      "PlaybackDev2",   // 0x8
      "RecordingDev3",  // 0x9
      "Tuner4",         // 0xA
      "PlaybackDev3",   // 0xB
      "Reserved",       // 0xC
      "Reserved",       // 0xD
      "SpecificUse",    // 0xE
      "Unregistered",   // 0xF
  };
  const char *dest = (frame_.is_broadcast()) ? "All" : names[frame_.destination_addr()];
  return std::string(names[frame_.initiator_addr()]) + " to " + dest + ": ";
}

const char *Decoder::find_opcode_name(uint32_t opcode) const {
  auto *entry = find_opcode(opcode);
  return entry ? entry->name : "?";
}

/**
 * Helper function to implement the 'do_operand' methods, to gather a textual representation.
 * @return true if a further operand can be decoded, false otherwise
 */
bool Decoder::append_operand(const char *word, uint8_t offset_incr /* default 1 */) {
  length_ += snprintf(&line_[length_], (line_.size() - length_), "[%s]", word);
  offset_ += offset_incr;
  return (length_ < line_.size()) && (offset_ < frame_.size());
}

/**
 * Entry function 'decode' to call for full decode of a CEC frame
 */
std::string Decoder::decode() {
  // src and dest fields
  std::string result = address_decode();
  // opcode field

  if (frame_.size() <= 1) {
    // Missing frame operation field?
    return result + "Ping";
  }

  auto *entry = find_opcode(frame_.opcode());
  if (entry == nullptr) {
    return result + std::string("<?>");
  }

  result += std::string("<") + entry->name + ">";
  // operand fields
  length_ = 0;         // currently accumulated length of text of operands
  line_[length_] = 0;  // initialise operand text to empty string
  offset_ = 2;         // location in frame of first operand to decode
  OperandDecode_f f = entry->decode_f;
  (this->*f)();
  result += &line_[0];
  return result;
}

}  // namespace hdmi_cec
}  // namespace esphome

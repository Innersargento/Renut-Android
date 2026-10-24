#pragma once
#include <rex/input/device_assignment.h>
#include <rex/input/input_driver.h>
#include <rex/input/input.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/virtual_key.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace renut {
using rex::X_RESULT;
using rex::X_STATUS;

// Players the driver feeds. Player 0 is the on-screen controller merged with the pad chosen for
// player one; player 1 is a second external pad and exists only while one is attached.
constexpr int kPads = 2;

struct TouchPad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Keystroke events (XamInputGetKeystroke*), which menus such as the Single
// Player house read instead of the pad state. Each change is queued, so
// a quick tap still yields both its down and up events.
using VK = rex::ui::VirtualKey;
constexpr std::array<VK, 34> kTouchKeys = {
    // 0-15: the XInput button bits
    VK::kXInputPadDpadUp, VK::kXInputPadDpadDown, VK::kXInputPadDpadLeft,
    VK::kXInputPadDpadRight, VK::kXInputPadStart, VK::kXInputPadBack,
    VK::kXInputPadLThumbPress, VK::kXInputPadRThumbPress, VK::kXInputPadLShoulder,
    VK::kXInputPadRShoulder, VK::kNone, VK::kNone, VK::kXInputPadA, VK::kXInputPadB,
    VK::kXInputPadX, VK::kXInputPadY,
    // 16-17: triggers
    VK::kXInputPadLTrigger, VK::kXInputPadRTrigger,
    // 18-25: left stick directions, 26-33: right stick directions
    VK::kXInputPadLThumbUp, VK::kXInputPadLThumbDown, VK::kXInputPadLThumbRight,
    VK::kXInputPadLThumbLeft, VK::kXInputPadLThumbUpLeft, VK::kXInputPadLThumbUpRight,
    VK::kXInputPadLThumbDownRight, VK::kXInputPadLThumbDownLeft,
    VK::kXInputPadRThumbUp, VK::kXInputPadRThumbDown, VK::kXInputPadRThumbRight,
    VK::kXInputPadRThumbLeft, VK::kXInputPadRThumbUpLeft, VK::kXInputPadRThumbUpRight,
    VK::kXInputPadRThumbDownRight, VK::kXInputPadRThumbDownLeft,
};
struct TouchKeyEvent {
  uint16_t key;
  uint16_t flags;
};

// Everything one guest user's pad keeps: the two sources, what the game sees, and its keystrokes.
struct Pad {
  TouchPad overlay;   // on-screen controller (player 0 only)
  TouchPad external;  // the Android gamepad assigned to this player
  TouchPad merged;    // what the game reads
  uint32_t packet = 0;
  std::deque<TouchKeyEvent> keys;
  uint64_t key_state = 0;
  int repeat_index = -1;
  std::chrono::steady_clock::time_point repeat_time;
};
inline std::mutex touch_mutex;
inline Pad pads[kPads];
// Player 0 is always there (the touch controller); player 1 follows the second gamepad.
inline bool pad_attached[kPads] = {true, false};

// What the game last asked of each player's rumble motors: 16 bits for the left (heavy) motor
// then 16 for the right (light) one, player 0 in the low 32 bits. The app reads it and drives the
// gamepad's motors, or the phone's when the pad has none.
inline std::atomic<uint64_t> rumble_packed{0};
inline void SetRumble(int player, uint16_t left, uint16_t right) {
  const int shift = player * 32;
  const uint64_t value = (uint64_t(left) | uint64_t(right) << 16) << shift;
  const uint64_t keep = ~(0xFFFFFFFFull << shift);
  uint64_t old = rumble_packed.load();
  while (!rumble_packed.compare_exchange_weak(old, (old & keep) | value)) {}
}

// Bit (0-7, in kTouchKeys order) of the direction a stick points to, or -1.
inline int StickDirection(int16_t x, int16_t y) {
  float fx = x / 32767.f, fy = y / 32767.f;
  if (fx * fx + fy * fy < 0.5f * 0.5f) return -1;
  // Eight 45-degree sectors starting at right, counter-clockwise.
  int sector = int(std::lround(std::atan2(fy, fx) / (3.14159265f / 4))) & 7;
  static constexpr int kBits[8] = {2, 5, 0, 4, 3, 7, 1, 6};  // R UR U UL L DL D DR
  return kBits[sector];
}

// Queues the key transitions since the last call. Caller holds touch_mutex.
inline void UpdatePadKeys(Pad& pad) {
  const TouchPad& now = pad.merged;
  uint64_t next = now.buttons;
  if (now.left_trigger > 64) next |= 1ull << 16;
  if (now.right_trigger > 64) next |= 1ull << 17;
  if (int d = StickDirection(now.lx, now.ly); d >= 0) next |= 1ull << (18 + d);
  if (int d = StickDirection(now.rx, now.ry); d >= 0) next |= 1ull << (26 + d);
  uint64_t changed = next ^ pad.key_state;
  // Releases first, so a stick moving between directions is never in two.
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < int(kTouchKeys.size()); ++i) {
      uint64_t bit = 1ull << i;
      if (!(changed & bit) || kTouchKeys[i] == VK::kNone) continue;
      bool down = next & bit;
      if (down != (pass == 1)) continue;
      if (pad.keys.size() < 64) {
        pad.keys.push_back({uint16_t(kTouchKeys[i]),
                            uint16_t(down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN
                                          : rex::input::X_INPUT_KEYSTROKE_KEYUP)});
      }
      if (down) {
        pad.repeat_index = i;
        pad.repeat_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
      } else if (pad.repeat_index == i) {
        pad.repeat_index = -1;
      }
    }
  }
  pad.key_state = next;
}

// XInput buttons the overlay may set: D-pad, Start, Back, L3, R3, LB, RB, A,
// B, X, Y. The Guide button is reserved by the system.
constexpr uint16_t kTouchButtonMask = 0xF3FF;

// Caller holds touch_mutex. External analog input takes priority while moved.
inline void UpdateMergedPad(Pad& pad) {
  const TouchPad& overlay = pad.overlay;
  const TouchPad& external = pad.external;
  TouchPad next;
  next.buttons = overlay.buttons | external.buttons;
  next.left_trigger = std::max(overlay.left_trigger, external.left_trigger);
  next.right_trigger = std::max(overlay.right_trigger, external.right_trigger);
  next.lx = external.lx ? external.lx : overlay.lx;
  next.ly = external.ly ? external.ly : overlay.ly;
  next.rx = external.rx ? external.rx : overlay.rx;
  next.ry = external.ry ? external.ry : overlay.ry;
  const TouchPad& was = pad.merged;
  if (next.buttons == was.buttons && next.left_trigger == was.left_trigger &&
      next.right_trigger == was.right_trigger && next.lx == was.lx && next.ly == was.ly &&
      next.rx == was.rx && next.ry == was.ry) return;
  pad.merged = next;
  ++pad.packet;
  UpdatePadKeys(pad);
}

inline void SetSourceButton(int player, bool external, uint16_t button, bool down) {
  if (player < 0 || player >= kPads) return;
  std::lock_guard lock(touch_mutex);
  Pad& pad = pads[player];
  TouchPad& source = external ? pad.external : pad.overlay;
  button &= kTouchButtonMask;
  source.buttons = down ? source.buttons | button : source.buttons & ~button;
  UpdateMergedPad(pad);
}

inline int16_t ToAxis(float value) {
  return int16_t(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
}

// stick 0 = left, 1 = right; x right-positive, y up-positive, both in [-1, 1].
inline void SetSourceStick(int player, bool external, int stick, float x, float y) {
  if (player < 0 || player >= kPads) return;
  std::lock_guard lock(touch_mutex);
  Pad& pad = pads[player];
  TouchPad& source = external ? pad.external : pad.overlay;
  int16_t& ax = stick ? source.rx : source.lx;
  int16_t& ay = stick ? source.ry : source.ly;
  ax = ToAxis(x);
  ay = ToAxis(y);
  UpdateMergedPad(pad);
}

// trigger 0 = left, 1 = right; value in [0, 1].
inline void SetSourceTrigger(int player, bool external, int trigger, float value) {
  if (player < 0 || player >= kPads) return;
  std::lock_guard lock(touch_mutex);
  Pad& pad = pads[player];
  TouchPad& source = external ? pad.external : pad.overlay;
  uint8_t& t = trigger ? source.right_trigger : source.left_trigger;
  t = uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
  UpdateMergedPad(pad);
}

inline void SetTouchButton(uint16_t button, bool down) { SetSourceButton(0, false, button, down); }
inline void SetTouchStick(int stick, float x, float y) { SetSourceStick(0, false, stick, x, y); }
inline void SetTouchTrigger(int trigger, float value) { SetSourceTrigger(0, false, trigger, value); }
inline void SetExternalButton(int player, uint16_t button, bool down) {
  SetSourceButton(player, true, button, down);
}
inline void SetExternalStick(int player, int stick, float x, float y) {
  SetSourceStick(player, true, stick, x, y);
}
inline void SetExternalTrigger(int player, int trigger, float value) {
  SetSourceTrigger(player, true, trigger, value);
}

// The second player's pad comes and goes with a gamepad. Detaching lets go of everything it held,
// so that a pad pulled out mid-press does not leave a button stuck for the next one. Returns
// whether anything changed.
inline bool SetPlayerAttached(int player, bool attached) {
  if (player < 1 || player >= kPads) return false;
  std::lock_guard lock(touch_mutex);
  if (pad_attached[player] == attached) return false;
  pad_attached[player] = attached;
  Pad& pad = pads[player];
  const uint32_t packet = pad.packet + 1;
  pad = Pad{};
  pad.packet = packet;
  if (!attached) SetRumble(player, 0, 0);
  return true;
}
inline bool PlayerAttached(int player) {
  if (player < 0 || player >= kPads) return false;
  std::lock_guard lock(touch_mutex);
  return pad_attached[player];
}

// The guest's users, from the driver's devices: the touch controller (and anything else that is
// not a second pad) is user 0, the second pad is user 1. Users 2 and 3 are never connected.
class PlayerAssignment final : public rex::input::DeviceAssignment {
 public:
  static constexpr const char* kSecondPadGuid = "renut-pad-2";

  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    for (auto& user : users_) user.clear();
    bool second = false;
    for (const auto& device : devices) {
      if (device.guid == kSecondPadGuid) {
        users_[1].push_back(device.id);
        second = true;
      } else {
        users_[0].push_back(device.id);
      }
    }
    // The title learns of a pad coming or going from the sign-in notification, whose parameter
    // is one bit per signed-in user, and re-reads the input state.
    const uint32_t mask = 1u | (second ? 2u : 0u);
    const uint32_t before = announced_.exchange(mask);
    if (before != 0 && before != mask) {
      if (auto* kernel = rex::system::kernel_state()) {
        kernel->BroadcastNotification(0x0000000A, mask);  // XN_SYS_SIGNINCHANGED
        kernel->BroadcastNotification(0x00000012, 0);     // XN_SYS_INPUTDEVICESCHANGED
      }
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index < users_.size()) out = users_[user_index];
  }

 private:
  std::array<std::vector<rex::input::DeviceId>, 2> users_;
  std::atomic<uint32_t> announced_{0};
};

// Combined controllers exposed to the game: player one's pad, and while a second gamepad is
// attached, player two's.
class TouchInput final : public rex::input::InputDriver {
  static constexpr auto id_ = static_cast<rex::input::DeviceId>(0x52454E5554);
  static constexpr auto id2_ = static_cast<rex::input::DeviceId>(0x52454E5555);

  static int PlayerOf(rex::input::DeviceId id) {
    return id == id_ ? 0 : id == id2_ ? 1 : -1;
  }

 public:
  TouchInput() : InputDriver(nullptr, 0) {}
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    out.push_back({id_, 0, "Renut touch", "renut-touch", true});
    if (PlayerAttached(1)) {
      out.push_back({id2_, 0, "Renut pad 2", PlayerAssignment::kSecondPadGuid, true});
    }
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    const int player = PlayerOf(id);
    if (player < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(touch_mutex);
    if (!pad_attached[player]) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      const Pad& pad = pads[player];
      *out = {};
      out->packet_number = pad.packet;
      out->gamepad.buttons = pad.merged.buttons;
      out->gamepad.left_trigger = pad.merged.left_trigger;
      out->gamepad.right_trigger = pad.merged.right_trigger;
      out->gamepad.thumb_lx = pad.merged.lx;
      out->gamepad.thumb_ly = pad.merged.ly;
      out->gamepad.thumb_rx = pad.merged.rx;
      out->gamepad.thumb_ry = pad.merged.ry;
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES* out) override {
    if (PlayerOf(id) < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      *out = {};
      out->type = 1;
      out->sub_type = 1;
      out->gamepad.buttons = kTouchButtonMask;
      out->gamepad.left_trigger = 0xFF;
      out->gamepad.right_trigger = 0xFF;
      out->gamepad.thumb_lx = int16_t(0xFFC0);
      out->gamepad.thumb_ly = int16_t(0xFFC0);
      out->gamepad.thumb_rx = int16_t(0xFFC0);
      out->gamepad.thumb_ry = int16_t(0xFFC0);
      // Both motors, so that titles which only rumble a pad that says it can do so do.
      out->vibration.left_motor_speed = 0xFFFF;
      out->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                                   rex::input::X_INPUT_VIBRATION* vibration) override {
    const int player = PlayerOf(id);
    if (player < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (vibration) {
      SetRumble(player, static_cast<uint16_t>(vibration->left_motor_speed),
                static_cast<uint16_t>(vibration->right_motor_speed));
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE* out) override {
    const int player = PlayerOf(id);
    if (player < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_BAD_ARGUMENTS;
    std::lock_guard lock(touch_mutex);
    if (!pad_attached[player]) return X_ERROR_DEVICE_NOT_CONNECTED;
    Pad& pad = pads[player];
    TouchKeyEvent event;
    if (!pad.keys.empty()) {
      event = pad.keys.front();
      pad.keys.pop_front();
    } else if (pad.repeat_index >= 0 &&
               std::chrono::steady_clock::now() >= pad.repeat_time) {
      // A held key repeats after 400 ms, every 100 ms, like a real pad.
      pad.repeat_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      event = {uint16_t(kTouchKeys[pad.repeat_index]),
               uint16_t(rex::input::X_INPUT_KEYSTROKE_KEYDOWN |
                        rex::input::X_INPUT_KEYSTROKE_REPEAT)};
    } else {
      return X_ERROR_EMPTY;
    }
    *out = {};
    out->virtual_key = event.key;
    out->flags = event.flags;
    return X_ERROR_SUCCESS;
  }
};
}  // namespace renut

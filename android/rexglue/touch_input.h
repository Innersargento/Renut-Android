#pragma once
#include <rex/input/input_driver.h>
#include <rex/input/input.h>
#include <rex/ui/virtual_key.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace renut {
using rex::X_RESULT;
using rex::X_STATUS;

// Merged state of the on-screen and external controllers.
struct TouchPad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
inline std::mutex touch_mutex;
inline TouchPad touch_pad;
inline TouchPad overlay_pad;
inline TouchPad external_pad;
inline uint32_t touch_packet = 0;

// Keystroke events (XamInputGetKeystroke*), which menus such as the Single
// Player house read instead of the pad state. Each touch change is queued, so
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
inline std::deque<TouchKeyEvent> touch_keys;
inline uint64_t touch_key_state = 0;
inline int touch_repeat_index = -1;
inline std::chrono::steady_clock::time_point touch_repeat_time;

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
inline void UpdateTouchKeys() {
  uint64_t next = touch_pad.buttons;
  if (touch_pad.left_trigger > 64) next |= 1ull << 16;
  if (touch_pad.right_trigger > 64) next |= 1ull << 17;
  if (int d = StickDirection(touch_pad.lx, touch_pad.ly); d >= 0) next |= 1ull << (18 + d);
  if (int d = StickDirection(touch_pad.rx, touch_pad.ry); d >= 0) next |= 1ull << (26 + d);
  uint64_t changed = next ^ touch_key_state;
  // Releases first, so a stick moving between directions is never in two.
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < int(kTouchKeys.size()); ++i) {
      uint64_t bit = 1ull << i;
      if (!(changed & bit) || kTouchKeys[i] == VK::kNone) continue;
      bool down = next & bit;
      if (down != (pass == 1)) continue;
      if (touch_keys.size() < 64) {
        touch_keys.push_back({uint16_t(kTouchKeys[i]),
                              uint16_t(down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN
                                            : rex::input::X_INPUT_KEYSTROKE_KEYUP)});
      }
      if (down) {
        touch_repeat_index = i;
        touch_repeat_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
      } else if (touch_repeat_index == i) {
        touch_repeat_index = -1;
      }
    }
  }
  touch_key_state = next;
}

// XInput buttons the overlay may set: D-pad, Start, Back, L3, R3, LB, RB, A,
// B, X, Y. The Guide button is reserved by the system.
constexpr uint16_t kTouchButtonMask = 0xF3FF;

// Caller holds touch_mutex. External analog input takes priority while moved.
inline void UpdateMergedPad() {
  TouchPad next;
  next.buttons = overlay_pad.buttons | external_pad.buttons;
  next.left_trigger = std::max(overlay_pad.left_trigger, external_pad.left_trigger);
  next.right_trigger = std::max(overlay_pad.right_trigger, external_pad.right_trigger);
  next.lx = external_pad.lx ? external_pad.lx : overlay_pad.lx;
  next.ly = external_pad.ly ? external_pad.ly : overlay_pad.ly;
  next.rx = external_pad.rx ? external_pad.rx : overlay_pad.rx;
  next.ry = external_pad.ry ? external_pad.ry : overlay_pad.ry;
  if (next.buttons == touch_pad.buttons && next.left_trigger == touch_pad.left_trigger &&
      next.right_trigger == touch_pad.right_trigger && next.lx == touch_pad.lx &&
      next.ly == touch_pad.ly && next.rx == touch_pad.rx && next.ry == touch_pad.ry) return;
  touch_pad = next;
  ++touch_packet;
  UpdateTouchKeys();
}

inline void SetSourceButton(TouchPad& source, uint16_t button, bool down) {
  std::lock_guard lock(touch_mutex);
  button &= kTouchButtonMask;
  source.buttons = down ? source.buttons | button : source.buttons & ~button;
  UpdateMergedPad();
}

inline int16_t ToAxis(float value) {
  return int16_t(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
}

// stick 0 = left, 1 = right; x right-positive, y up-positive, both in [-1, 1].
inline void SetSourceStick(TouchPad& source, int stick, float x, float y) {
  std::lock_guard lock(touch_mutex);
  int16_t& ax = stick ? source.rx : source.lx;
  int16_t& ay = stick ? source.ry : source.ly;
  ax = ToAxis(x);
  ay = ToAxis(y);
  UpdateMergedPad();
}

// trigger 0 = left, 1 = right; value in [0, 1].
inline void SetSourceTrigger(TouchPad& source, int trigger, float value) {
  std::lock_guard lock(touch_mutex);
  uint8_t& t = trigger ? source.right_trigger : source.left_trigger;
  t = uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
  UpdateMergedPad();
}

inline void SetTouchButton(uint16_t button, bool down) { SetSourceButton(overlay_pad, button, down); }
inline void SetTouchStick(int stick, float x, float y) { SetSourceStick(overlay_pad, stick, x, y); }
inline void SetTouchTrigger(int trigger, float value) { SetSourceTrigger(overlay_pad, trigger, value); }
inline void SetExternalButton(uint16_t button, bool down) { SetSourceButton(external_pad, button, down); }
inline void SetExternalStick(int stick, float x, float y) { SetSourceStick(external_pad, stick, x, y); }
inline void SetExternalTrigger(int trigger, float value) { SetSourceTrigger(external_pad, trigger, value); }

// Combined controller exposed to the game as player one's gamepad.
class TouchInput final : public rex::input::InputDriver {
  static constexpr auto id_ = static_cast<rex::input::DeviceId>(0x52454E5554);

 public:
  TouchInput() : InputDriver(nullptr, 0) {}
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    out.push_back({id_, 0, "Renut touch", "renut-touch", true});
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(touch_mutex);
    if (out) {
      *out = {};
      out->packet_number = touch_packet;
      out->gamepad.buttons = touch_pad.buttons;
      out->gamepad.left_trigger = touch_pad.left_trigger;
      out->gamepad.right_trigger = touch_pad.right_trigger;
      out->gamepad.thumb_lx = touch_pad.lx;
      out->gamepad.thumb_ly = touch_pad.ly;
      out->gamepad.thumb_rx = touch_pad.rx;
      out->gamepad.thumb_ry = touch_pad.ry;
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
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
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == id_ ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE* out) override {
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_BAD_ARGUMENTS;
    std::lock_guard lock(touch_mutex);
    TouchKeyEvent event;
    if (!touch_keys.empty()) {
      event = touch_keys.front();
      touch_keys.pop_front();
    } else if (touch_repeat_index >= 0 &&
               std::chrono::steady_clock::now() >= touch_repeat_time) {
      // A held key repeats after 400 ms, every 100 ms, like a real pad.
      touch_repeat_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      event = {uint16_t(kTouchKeys[touch_repeat_index]),
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

#pragma once
#include <rex/input/input_driver.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace renut {
using rex::X_RESULT;
using rex::X_STATUS;

// State of the on-screen Xbox 360 controller, written by the Java overlay.
struct TouchPad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
inline std::mutex touch_mutex;
inline TouchPad touch_pad;
inline uint32_t touch_packet = 0;

// XInput buttons the overlay may set: D-pad, Start, Back, L3, R3, LB, RB, A,
// B, X, Y. The Guide button is reserved by the system.
constexpr uint16_t kTouchButtonMask = 0xF3FF;

inline void SetTouchButton(uint16_t button, bool down) {
  std::lock_guard lock(touch_mutex);
  button &= kTouchButtonMask;
  uint16_t next = down ? touch_pad.buttons | button : touch_pad.buttons & ~button;
  if (next != touch_pad.buttons) {
    touch_pad.buttons = next;
    ++touch_packet;
  }
}

inline int16_t ToAxis(float value) {
  return int16_t(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
}

// stick 0 = left, 1 = right; x right-positive, y up-positive, both in [-1, 1].
inline void SetTouchStick(int stick, float x, float y) {
  std::lock_guard lock(touch_mutex);
  int16_t& ax = stick ? touch_pad.rx : touch_pad.lx;
  int16_t& ay = stick ? touch_pad.ry : touch_pad.ly;
  int16_t nx = ToAxis(x), ny = ToAxis(y);
  if (nx != ax || ny != ay) {
    ax = nx;
    ay = ny;
    ++touch_packet;
  }
}

// trigger 0 = left, 1 = right; value in [0, 1].
inline void SetTouchTrigger(int trigger, float value) {
  std::lock_guard lock(touch_mutex);
  uint8_t& t = trigger ? touch_pad.right_trigger : touch_pad.left_trigger;
  auto next = uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
  if (next != t) {
    t = next;
    ++touch_packet;
  }
}

// On-screen controller exposed to the game as player one's gamepad.
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
                                   rex::input::X_INPUT_KEYSTROKE*) override {
    return id == id_ ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};
}  // namespace renut

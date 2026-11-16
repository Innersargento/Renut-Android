#pragma once
#include <string_view>

#include <rex/driver_context.h>
#include <rex/hid/input_driver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace renut {

// Players the driver feeds. Player 0 is the on-screen controller merged with the pad chosen for
// player one; player 1 is a second external pad and exists only while one is attached.
constexpr int kPads = 2;

struct TouchPad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Everything one guest user's pad keeps: the two sources, what the game sees, and its keystrokes.
struct Pad {
  TouchPad overlay;   // on-screen controller (player 0 only)
  TouchPad external;  // the Android gamepad assigned to this player
  TouchPad merged;    // what the game reads
  uint32_t packet = 0;
};
inline std::mutex touch_mutex;
inline Pad pads[kPads];
// Player 0 is always there (the touch controller); player 1 follows the second gamepad.
inline bool pad_attached[kPads] = {true, false};

// What the game last asked of each player's rumble motors: 16 bits for the left (heavy) motor
// then 16 for the right (light) one, player 0 in the low 32 bits. The app reads it and drives the
// gamepad's motors, or the phone's when the pad has none.
inline std::atomic<uint64_t> rumble_packed{0};
// Changes whenever a pad comes or goes: the input system asks for the list of devices again.
inline std::atomic<uint64_t> pad_revision{1};
inline void SetRumble(int player, uint16_t left, uint16_t right) {
  const int shift = player * 32;
  const uint64_t value = (uint64_t(left) | uint64_t(right) << 16) << shift;
  const uint64_t keep = ~(0xFFFFFFFFull << shift);
  uint64_t old = rumble_packed.load();
  while (!rumble_packed.compare_exchange_weak(old, (old & keep) | value)) {}
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
  pad_revision.fetch_add(1, std::memory_order_acq_rel);
  return true;
}
inline bool PlayerAttached(int player) {
  if (player < 0 || player >= kPads) return false;
  std::lock_guard lock(touch_mutex);
  return pad_attached[player];
}

// Combined controllers exposed to the game: player one's pad, and while a second gamepad is
// attached, player two's. The pads say which user they are (the XInput slot of their identity), so the
// SDK's input system assigns them without the app's help.
class TouchInput final : public rex::hid::IInputDriver {
  static constexpr uint64_t kId0 = 0x52454E5554;
  static constexpr uint64_t kId1 = 0x52454E5555;

  static int PlayerOf(rex::hid::PadDeviceID id) {
    return id.value == kId0 ? 0 : id.value == kId1 ? 1 : -1;
  }

 public:
  std::string_view GetName() const override { return "touch"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return rex::Ok(); }
  void Shutdown() override {}

  // A second pad coming or going changes the list of devices.
  uint64_t GetDeviceRevision() const override { return pad_revision.load(std::memory_order_acquire); }

  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override {
    rex::hid::PadDevice touch;
    touch.id = {kId0};
    touch.name = "Renut touch";
    touch.info.type = rex::hid::PadType::kX360;
    touch.identity.xinput_slot = 0;
    out.push_back(touch);
    if (PlayerAttached(1)) {
      rex::hid::PadDevice second;
      second.id = {kId1};
      second.name = "Renut pad 2";
      second.info.type = rex::hid::PadType::kX360;
      second.identity.xinput_slot = 1;
      out.push_back(second);
    }
  }

  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override {
    const int player = PlayerOf(id);
    if (player < 0) return rex::Err<rex::hid::PadState>(rex::ErrorCategory::kNotFound, "no such pad");
    std::lock_guard lock(touch_mutex);
    if (!pad_attached[player]) return rex::Err<rex::hid::PadState>(rex::ErrorCategory::kNotFound, "pad detached");
    const Pad& pad = pads[player];
    rex::hid::PadState state;
    state.buttons = pad.merged.buttons;
    state.left_trigger = pad.merged.left_trigger;
    state.right_trigger = pad.merged.right_trigger;
    state.thumb_lx = pad.merged.lx;
    state.thumb_ly = pad.merged.ly;
    state.thumb_rx = pad.merged.rx;
    state.thumb_ry = pad.merged.ry;
    return state;
  }

  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID id) override {
    if (PlayerOf(id) < 0) return rex::Err<rex::hid::PadCapabilities>(rex::ErrorCategory::kNotFound, "no such pad");
    rex::hid::PadCapabilities capabilities;
    capabilities.supported.buttons = kTouchButtonMask;
    capabilities.supported.left_trigger = 0xFF;
    capabilities.supported.right_trigger = 0xFF;
    capabilities.supported.thumb_lx = int16_t(0xFFC0);
    capabilities.supported.thumb_ly = int16_t(0xFFC0);
    capabilities.supported.thumb_rx = int16_t(0xFFC0);
    capabilities.supported.thumb_ry = int16_t(0xFFC0);
    // Both motors, so that titles which only rumble a pad that says it can do so do.
    capabilities.motors.left_motor = 0xFFFF;
    capabilities.motors.right_motor = 0xFFFF;
    return capabilities;
  }

  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID id, const rex::hid::PadVibration& vibration) override {
    const int player = PlayerOf(id);
    if (player < 0) return rex::Err(rex::ErrorCategory::kNotFound, "no such pad");
    SetRumble(player, vibration.left_motor, vibration.right_motor);
    return rex::Ok();
  }
};
}  // namespace renut

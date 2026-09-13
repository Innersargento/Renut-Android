#pragma once
#include <rex/input/input_driver.h>
#include <mutex>

namespace renut {
using rex::X_RESULT;
using rex::X_STATUS;
inline std::mutex touch_mutex;
inline uint16_t touch_buttons=0;
inline uint32_t touch_packet=0;
inline void SetTouchButton(uint16_t button,bool down) {
  std::lock_guard lock(touch_mutex);
  uint16_t next=down ? touch_buttons|button : touch_buttons&~button;
  if(next!=touch_buttons){touch_buttons=next;++touch_packet;}
}
// Menu controls used to advance the real game during Android validation.
class TouchInput final : public rex::input::InputDriver {
  static constexpr auto id_=static_cast<rex::input::DeviceId>(0x52454E5554);
 public:
  TouchInput():InputDriver(nullptr,0){}
  rex::X_STATUS Setup() override{return X_STATUS_SUCCESS;}
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    out.push_back({id_,0,"Renut touch","renut-touch",true});
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,rex::input::X_INPUT_STATE* out) override {
    if(id!=id_)return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(touch_mutex);
    if(out){*out={};out->packet_number=touch_packet;out->gamepad.buttons=touch_buttons;}
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id,uint32_t,rex::input::X_INPUT_CAPABILITIES* out) override {
    if(id!=id_)return X_ERROR_DEVICE_NOT_CONNECTED;
    if(out){*out={};out->type=1;out->sub_type=1;out->gamepad.buttons=0x301F;}
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,rex::input::X_INPUT_VIBRATION*) override {
    return id==id_?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id,uint32_t,rex::input::X_INPUT_KEYSTROKE*) override {
    return id==id_?X_ERROR_EMPTY:X_ERROR_DEVICE_NOT_CONNECTED;
  }
};
}

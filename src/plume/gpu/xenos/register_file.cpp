/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu), so that the renderer does not
 *              depend on the SDK's Xenos GPU emulation
 */

#include <cstring>

#include "register_file.h"
#include <rex/math.h>

namespace renut::gpu {


RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));
}

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_GPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterInfo reg_info = { \
        RegisterInfo::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include "register_table.inc"
#undef XE_GPU_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace renut::gpu

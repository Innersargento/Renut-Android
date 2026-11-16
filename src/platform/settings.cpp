// The app's options: see settings.h.
#include "platform/settings.h"

namespace renut {

Settings& GetSettings() {
  static Settings settings;
  return settings;
}

}  // namespace renut

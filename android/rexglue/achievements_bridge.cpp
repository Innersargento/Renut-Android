// Achievements: exports the title's catalog for the launcher and reports unlocks to the game screen.
//
// The SDK already keeps the catalog (read from the XEX's XDBF), persists unlocks to
// runtime-user/achievements/<TITLEID>.toml and calls a notification callback when the
// guest writes one. This file only hands that over to Java: the catalog and the
// icons go to <files>/achievements/ once the XEX is loaded (the launcher reads them
// together with the unlock file), and a callback tells RuntimeActivity which id was won.
#include <jni.h>
#include <android/log.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <fmt/format.h>

namespace {

JavaVM* java_vm = nullptr;
jclass activity_class = nullptr;
jmethodID unlocked_method = nullptr;
std::mutex bridge_mutex;
rex::system::AchievementListenerHandle listener = 0;

void appendJsonString(std::string& out, const std::string& value) {
  out += '"';
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) out += fmt::format("\\u{:04x}", c);
        else out += char(c);
    }
  }
  out += '"';
}

// A guest thread reports the unlock; it is attached to the VM for the call and detached
// when the thread ends, so the VM never keeps a reference to a dead thread.
struct ThreadAttachment {
  bool attached = false;
  ~ThreadAttachment() { if (attached && java_vm) java_vm->DetachCurrentThread(); }
};

void notifyJava(uint32_t id) {
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (!java_vm || !activity_class || !unlocked_method) return;
  JNIEnv* env = nullptr;
  thread_local ThreadAttachment attachment;
  if (java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attachment.attached = true;
  }
  if (!env) return;
  env->CallStaticVoidMethod(activity_class, unlocked_method, jint(id));
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

void writeCatalog(rex::Runtime& runtime, const std::filesystem::path& directory) {
  auto* kernel = runtime.kernel_state();
  const auto achievements = kernel->loaded_achievements();
  const auto database = kernel->title_xdbf();
  std::error_code ec;
  std::filesystem::create_directories(directory / "icons", ec);
  if (ec) {
    REXSYS_WARN("Achievements: cannot create {}: {}", directory.string(), ec.message());
    return;
  }
  std::string json = fmt::format("{{\"titleId\":\"{:08X}\",\"achievements\":[", kernel->title_id());
  bool first = true;
  for (const auto& info : achievements) {
    if (!first) json += ',';
    first = false;
    json += fmt::format("{{\"id\":{},\"gamerscore\":{},\"flags\":{},\"image\":{},\"label\":",
                        info.id, info.gamerscore, info.flags, info.image_id);
    appendJsonString(json, info.label);
    json += ",\"description\":";
    appendJsonString(json, info.description);
    json += ",\"lockedDescription\":";
    appendJsonString(json, info.unachieved_description);
    json += '}';
    if (!database.is_valid()) continue;
    const auto icon = database.GetEntry(rex::system::util::XdbfSection::kImage, info.image_id);
    if (!icon) continue;
    const auto path = directory / "icons" / fmt::format("{}.png", info.image_id);
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == icon.size) continue;
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(icon.buffer), std::streamsize(icon.size));
  }
  json += "]}";
  const auto temporary = directory / "catalog.json.tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << json;
    if (!file) {
      REXSYS_WARN("Achievements: cannot write {}", temporary.string());
      return;
    }
  }
  std::filesystem::rename(temporary, directory / "catalog.json", ec);
  if (ec) REXSYS_WARN("Achievements: rename failed: {}", ec.message());
  else REXSYS_INFO("Achievements: {} entries exported", achievements.size());
}

}  // namespace

namespace renut {

// Called with the XEX loaded (the catalog is read from it). `activity` supplies the
// class whose static onAchievementUnlocked(int) receives the unlocks.
void AchievementsStart(JNIEnv* env, jobject activity, rex::Runtime& runtime,
                       const std::filesystem::path& files_directory) {
  try {
    writeCatalog(runtime, files_directory / "achievements");
  } catch (const std::exception& error) {
    REXSYS_WARN("Achievements: catalog not exported: {}", error.what());
  }
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (!java_vm) env->GetJavaVM(&java_vm);
  if (!activity_class) {
    jclass local = env->GetObjectClass(activity);
    activity_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    unlocked_method = env->GetStaticMethodID(activity_class, "onAchievementUnlocked", "(I)V");
    if (!unlocked_method) env->ExceptionClear();
  }
  if (listener) return;
  listener = runtime.kernel_state()->achievements().RegisterNotificationCallback(
      [](const rex::system::AchievementEvent& event) { notifyJava(event.achievement.id); });
}

// Before the runtime is torn down: no guest thread may call into Java afterwards.
void AchievementsStop(rex::Runtime& runtime) {
  rex::system::AchievementListenerHandle handle;
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    handle = listener;
    listener = 0;
  }
  if (handle && runtime.kernel_state()) runtime.kernel_state()->achievements().UnregisterCallback(handle);
}

}  // namespace renut

// Achievements: exports the title's catalog for the launcher and reports unlocks to the game screen.
//
// The SDK already keeps the catalog (read from the XEX's XDBF), persists unlocks to
// runtime-user/achievements/<TITLEID>.toml and calls a notification callback when the
// guest writes one. This file only hands that over to Java: the catalog and the
// icons go to <files>/achievements/ once the XEX is loaded (the launcher reads them
// together with the unlock file), and a callback tells RuntimeActivity which id was won.
#include <jni.h>
#include <android/log.h>
#include <rex/filesystem/xdbf.h>
#include <rex/xam/achievement_manager.h>
#include "platform/log.h"
#include "platform/guest.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <format>

namespace {

JavaVM* java_vm = nullptr;
jclass activity_class = nullptr;
jmethodID unlocked_method = nullptr;
std::mutex bridge_mutex;
rex::ScopeGuard listener;
bool listening = false;

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
        if (c < 0x20) out += std::format("\\u{:04x}", c);
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

void writeImage(const std::filesystem::path& path, const rex::filesystem::XDBFBlock& image) {
  std::error_code ec;
  if (!image) return;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == image.size) return;
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(image.buffer), std::streamsize(image.size));
}

void writeCatalog(rex::kernel::TitleRuntime& title, const std::filesystem::path& directory) {
  const auto achievements = title.GetAchievementManager().ListAchievements();
  const auto database_owner = title.GetTitleXDBF();
  const rex::filesystem::IXDBFTitleData* database = database_owner.get();
  std::error_code ec;
  std::filesystem::create_directories(directory / "icons", ec);
  if (ec) {
    REXLOG_CAT_WARN(rex::log::renut, "Achievements: cannot create {}: {}", directory.string(), ec.message());
    return;
  }
  std::string json = std::format("{{\"titleId\":\"{:08X}\",\"achievements\":[", title.GetTitleID());
  bool first = true;
  for (const auto& info : achievements) {
    if (!first) json += ',';
    first = false;
    json += std::format("{{\"id\":{},\"gamerscore\":{},\"flags\":{},\"image\":{},\"label\":",
                        info.id, info.gamerscore, info.flags, info.image_id);
    appendJsonString(json, info.label);
    json += ",\"description\":";
    appendJsonString(json, info.description);
    json += ",\"lockedDescription\":";
    appendJsonString(json, info.unachieved_description);
    json += '}';
    if (!database) continue;
    writeImage(directory / "icons" / std::format("{}.png", info.image_id),
               database->GetEntry(rex::filesystem::XDBFSection::kImage, info.image_id));
  }
  // The game's own cover, for the launcher's play screen.
  if (database) writeImage(directory / "title.png", database->GetIcon());
  json += "]}";
  const auto temporary = directory / "catalog.json.tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << json;
    if (!file) {
      REXLOG_CAT_WARN(rex::log::renut, "Achievements: cannot write {}", temporary.string());
      return;
    }
  }
  std::filesystem::rename(temporary, directory / "catalog.json", ec);
  if (ec) REXLOG_CAT_WARN(rex::log::renut, "Achievements: rename failed: {}", ec.message());
  else REXLOG_CAT_INFO(rex::log::renut, "Achievements: {} entries exported", achievements.size());
}

}  // namespace

namespace renut {

// Called with the XEX loaded (the catalog is read from it). `activity` supplies the
// class whose static onAchievementUnlocked(int) receives the unlocks.
void AchievementsStart(JNIEnv* env, jobject activity, rex::kernel::TitleRuntime& title,
                       const std::filesystem::path& files_directory) {
  try {
    writeCatalog(title, files_directory / "achievements");
  } catch (const std::exception& error) {
    REXLOG_CAT_WARN(rex::log::renut, "Achievements: catalog not exported: {}", error.what());
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
  if (listening) return;
  listening = true;
  listener = title.GetAchievementManager().RegisterNotificationCallback(
      [](const rex::xam::AchievementEvent& event) { notifyJava(event.achievement.id); });
}

// Before the runtime is torn down: no guest thread may call into Java afterwards.
void AchievementsStop() {
  rex::ScopeGuard handle;
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    handle = std::move(listener);
    listening = false;
  }
  handle.Reset();
}

}  // namespace renut

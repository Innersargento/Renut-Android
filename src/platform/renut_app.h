// The app on the SDK: a rex::app::TitleApp that runs the recompiled game with the app's own drivers
// (android_drivers.h) and settings (settings.h).
#pragma once
#include <jni.h>

#include <filesystem>
#include <string>

namespace renut {

// What the activity hands over to start a session (RuntimeActivity.nativeValidate).
struct Session {
  JNIEnv* env = nullptr;
  jobject activity = nullptr;
  std::filesystem::path root;      // the app's files folder
  std::filesystem::path game;      // the imported game: default.xex and Bundle/
  std::string log_path;            // the runtime log, or a file the player asked for
  bool launch = false;             // false: load the executable and check it only
};

// Runs the session on the calling thread until the game ends or the activity stops it. Returns what
// happened, one line per step (the runtime report the activity shows).
void RunSession(Session& session);

// The player left the game (nativeStop): the session ends at its next turn of the loop.
void RequestStop();

}  // namespace renut

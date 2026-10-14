#include <jni.h>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace {
JavaVM* vm = nullptr;
jobject activity = nullptr;
std::mutex content_mutex;
}

void renutSetContentActivity(JNIEnv* env, jobject value) {
  std::lock_guard lock(content_mutex);
  env->GetJavaVM(&vm);
  if (activity) env->DeleteGlobalRef(activity);
  activity = value ? env->NewGlobalRef(value) : nullptr;
}

namespace rex::filesystem {
int OpenAndroidContentFileDescriptor(std::string_view uri, const char* mode) {
  std::lock_guard lock(content_mutex);
  if (!vm || !activity) { errno = ENODEV; return -1; }
  JNIEnv* env = nullptr;
  bool attached = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
  if (attached && vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { errno = EIO; return -1; }
  jclass cls = env->GetObjectClass(activity);
  jmethodID open = env->GetMethodID(cls, "openContentFd", "(Ljava/lang/String;Ljava/lang/String;)I");
  auto path = env->NewStringUTF(std::string(uri).c_str());
  auto access = env->NewStringUTF(mode);
  int fd = -1;
  if (open && path && access && !env->ExceptionCheck()) fd = env->CallIntMethod(activity, open, path, access);
  if (env->ExceptionCheck()) { env->ExceptionClear(); fd = -1; }
  env->DeleteLocalRef(path); env->DeleteLocalRef(access); env->DeleteLocalRef(cls);
  if (attached) vm->DetachCurrentThread();
  if (fd < 0) errno = EIO;
  return fd;
}
}

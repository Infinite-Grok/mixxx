#pragma once
#include <libremidi/error.hpp>

#include <amidi/AMidi.h>
#include <android/log.h>

#include <jni.h>
#include <pthread.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "libremidi"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

NAMESPACE_LIBREMIDI
{
namespace android
{
//! A message to give to the user's on_error / on_warning callback.
struct report
{
  bool is_error{};
  std::string text;
};

//! A port that is waiting for (or owns) an AMidiDevice which Java opens asynchronously.
struct target
{
  virtual ~target() = default;

  //! The device is open. Takes ownership of `device`. Runs on the thread that delivers
  //! MidiManager.OnDeviceOpenedListener (the main looper), with the registry lock held:
  //! must not call user callbacks, queue them in `out` instead.
  virtual void on_device_opened(AMidiDevice* device, std::vector<report>& out) = 0;

  //! MidiManager.openDevice reported a failure (device == null). Same rules as above.
  virtual void on_open_failed(std::vector<report>& out) = 0;
};

//! Relaxed store to a plain bool owned by libremidi::midi_api (public is_port_open()).
inline void store_relaxed(bool& b, bool v) noexcept
{
  __atomic_store_n(&b, v, __ATOMIC_RELAXED);
}

struct context
{
  static inline std::string client_name;
  static inline std::vector<jobject> midi_devices;

  static JNIEnv* get_thread_env();
  static jobject get_context(JNIEnv* env);
  static jobject get_midi_manager(JNIEnv* env, jobject context);
  static void refresh_midi_devices(JNIEnv* env, jobject context, bool is_output);
  //! Look a class of the application up through its class loader: FindClass on a thread
  //! attached from native code only sees the system class loader.
  static jclass find_app_class(JNIEnv* env, jobject context, const char* dotted_name);
  //! Starts MidiManager.openDevice. `cookie` is handed back to
  //! handle_device_opened() by Java; it is NOT a pointer. Returns false if the request
  //! could not be issued (nothing will be delivered then).
  static bool open_device(jobject device_info, void* cookie, bool is_output);
  static std::string port_name(JNIEnv* env, unsigned int port_number);
  static void cleanup_devices(JNIEnv* env);

  // ---- Lifetime of asynchronous opens ----
  // The Java MidiDeviceCallback may run long after open_port(), and even after the port
  // was closed or destroyed. It therefore never gets a pointer: it gets a token that is
  // only valid while the port is registered here. The registry lock is held while a
  // target is being called, so unregister_target() returns only when no call is in flight.
  struct target_entry
  {
    target* t{};
    midi_error_callback on_error;
    midi_warning_callback on_warning;
  };
  static inline std::recursive_mutex registry_mutex;
  static inline std::unordered_map<jlong, target_entry> targets;
  static inline jlong last_token = 0;

  static jlong register_target(
      target* t, const midi_error_callback& on_error, const midi_warning_callback& on_warning)
  {
    std::lock_guard l{registry_mutex};
    const jlong token = ++last_token;
    targets[token] = target_entry{t, on_error, on_warning};
    return token;
  }

  static void unregister_target(jlong token)
  {
    std::lock_guard l{registry_mutex};
    targets.erase(token);
  }

  //! Java side of MidiManager.OnDeviceOpenedListener. `device` is null on failure.
  //! Takes ownership of `device`; releases it if the port is gone by now.
  static void handle_device_opened(AMidiDevice* device, jlong token)
  {
    std::vector<report> reports;
    midi_error_callback on_error;
    midi_warning_callback on_warning;
    {
      std::lock_guard l{registry_mutex};
      auto it = targets.find(token);
      if (it == targets.end())
      {
        // The port was closed or destroyed while the device was opening.
        if (device)
          AMidiDevice_release(device);
        return;
      }
      on_error = it->second.on_error;
      on_warning = it->second.on_warning;
      if (device)
        it->second.t->on_device_opened(device, reports);
      else
        it->second.t->on_open_failed(reports);
    }

    // User callbacks run without the lock so that they may close / reopen ports.
    for (const auto& r : reports)
    {
      const auto& cb = r.is_error ? on_error : on_warning;
      if (cb)
        cb(r.text, libremidi::source_location::current());
    }
  }
};

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong target_ptr, jboolean is_output);
extern "C" JNIEXPORT void JNICALL
Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpenFailed(
    JNIEnv* env, jobject /*thiz*/, jlong target_ptr, jboolean is_output);
}
}

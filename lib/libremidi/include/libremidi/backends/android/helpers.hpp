#pragma once
#include <libremidi/error.hpp>
#include <libremidi/observer_configuration.hpp>

#include <amidi/AMidi.h>
#include <android/log.h>

#include <jni.h>
#include <pthread.h>

#include <algorithm>
#include <cstdint>
#include <memory>
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

  //! The device this port was opened on was removed (MidiManager.DeviceCallback.onDeviceRemoved).
  //! Same rules as above; may be called more than once.
  virtual void on_device_lost(std::vector<report>& out) = 0;
};

//! What the Java side knows about a MidiDeviceInfo.
struct device_event
{
  int id{};            //! MidiDeviceInfo.getId(): changes when a device is replugged
  std::string name;    //! MidiDeviceInfo.getProperties().getString("name")
  int input_ports{};   //! MidiDeviceInfo.getInputPortCount(): ports we send to (libremidi outputs)
  int output_ports{};  //! MidiDeviceInfo.getOutputPortCount(): ports we read from (libremidi inputs)
};

//! The callbacks of one android::observer, shared with the hotplug dispatcher so that a
//! notification can never reach an observer that is being destroyed.
struct observer_sink
{
  std::recursive_mutex mutex;
  bool dead{};
  input_port_callback input_added, input_removed;
  output_port_callback output_added, output_removed;
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
  //! MidiDeviceInfo.getId() of each entry of midi_devices
  static inline std::vector<int> midi_device_ids;

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

  //! All devices known to MidiManager.getDevices(), in the same order refresh_midi_devices()
  //! walks them. Does not touch midi_devices, so it is safe from any thread.
  static std::vector<device_event> list_devices(JNIEnv* env, jobject context);
  //! Reads a MidiDeviceInfo.
  static device_event read_device_event(JNIEnv* env, jobject device_info);

  //! Register / unregister (reference counted) the process-wide
  //! MidiManager.DeviceCallback that feeds notify_device_added() / notify_device_removed().
  //! Needs the DeviceObserver Java class in the application.
  static void acquire_device_callback();
  static void release_device_callback();

  // ---- Lifetime of asynchronous opens ----
  // The Java MidiDeviceCallback may run long after open_port(), and even after the port
  // was closed or destroyed. It therefore never gets a pointer: it gets a token that is
  // only valid while the port is registered here. The registry lock is held while a
  // target is being called, so unregister_target() returns only when no call is in flight.
  struct target_entry
  {
    target* t{};
    int device_id{}; // MidiDeviceInfo.getId() of the device the port was opened on
    midi_error_callback on_error;
    midi_warning_callback on_warning;
  };
  static inline std::recursive_mutex registry_mutex;
  static inline std::unordered_map<jlong, target_entry> targets;
  static inline jlong last_token = 0;

  //! Registers a port that is opening `device_id`. While registered, the process listens for
  //! the removal of that device (acquire_device_callback()).
  static jlong register_target(
      target* t, int device_id, const midi_error_callback& on_error,
      const midi_warning_callback& on_warning)
  {
    jlong token;
    {
      std::lock_guard l{registry_mutex};
      token = ++last_token;
      targets[token] = target_entry{t, device_id, on_error, on_warning};
    }
    acquire_device_callback();
    return token;
  }

  static void unregister_target(jlong token)
  {
    bool erased;
    {
      std::lock_guard l{registry_mutex};
      erased = targets.erase(token) > 0;
    }
    if (erased)
      release_device_callback();
  }

  //! Runs the user callbacks for `reports`. Never call this with a lock held.
  static void deliver_reports(
      const std::vector<report>& reports, const midi_error_callback& on_error,
      const midi_warning_callback& on_warning)
  {
    for (const auto& r : reports)
    {
      const auto& cb = r.is_error ? on_error : on_warning;
      if (cb)
        cb(r.text, libremidi::source_location::current());
    }
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
    deliver_reports(reports, on_error, on_warning);
  }

  // ---- Hotplug ----
  // MidiManager.DeviceCallback (Java class DeviceObserver) calls notify_device_added() /
  // notify_device_removed() on the main looper. `current` is MidiManager.getDevices().
  static inline std::vector<std::shared_ptr<observer_sink>> observers;
  static inline std::vector<device_event> hotplug_snapshot; // devices as of the last event

  static void register_observer(const std::shared_ptr<observer_sink>& sink)
  {
    {
      std::lock_guard l{registry_mutex};
      observers.push_back(sink);
    }
    acquire_device_callback();
  }

  //! Returns when no notification to `sink` is in flight; none is started afterwards.
  static void unregister_observer(const std::shared_ptr<observer_sink>& sink)
  {
    {
      std::lock_guard l{registry_mutex};
      std::erase(observers, sink);
    }
    {
      std::lock_guard l{sink->mutex};
      sink->dead = true;
    }
    release_device_callback();
  }

  template <typename Port>
  static Port make_port(const device_event& ev, std::size_t index)
  {
    Port p;
    p.api = libremidi::API::ANDROID_AMIDI;
    p.port_name = ev.name;
    p.port = index;
    p.device = static_cast<std::uint64_t>(ev.id);
    return p;
  }

  //! Index of `id` among the devices that have input (libremidi outputs) or output
  //! (libremidi inputs) ports, i.e. its index in midi_devices after refresh_midi_devices().
  static std::size_t port_index(const std::vector<device_event>& list, int id, bool is_output)
  {
    std::size_t index = 0;
    for (const auto& d : list)
    {
      if (d.id == id)
        break;
      if ((is_output ? d.input_ports : d.output_ports) > 0)
        index++;
    }
    return index;
  }

  //! Tells the observers about the ports of `ev`. `list` is the device list that the port
  //! indices refer to.
  static void notify_observers(bool added, const device_event& ev, const std::vector<device_event>& list)
  {
    std::vector<std::shared_ptr<observer_sink>> sinks;
    {
      std::lock_guard l{registry_mutex};
      sinks = observers;
    }
    const bool has_in = ev.output_ports > 0, has_out = ev.input_ports > 0;
    const auto in = make_port<libremidi::input_port>(ev, port_index(list, ev.id, false));
    const auto out = make_port<libremidi::output_port>(ev, port_index(list, ev.id, true));
    for (const auto& s : sinks)
    {
      // Without the registry lock, so that callbacks may open and close ports.
      std::lock_guard l{s->mutex};
      if (s->dead)
        continue;
      if (has_in)
        if (const auto& cb = added ? s->input_added : s->input_removed)
          cb(in);
      if (has_out)
        if (const auto& cb = added ? s->output_added : s->output_removed)
          cb(out);
    }
  }

  static void notify_device_added(const device_event& ev, std::vector<device_event> current)
  {
    if (std::none_of(current.begin(), current.end(), [&](auto& d) { return d.id == ev.id; }))
      current.push_back(ev);
    {
      std::lock_guard l{registry_mutex};
      hotplug_snapshot = current;
    }
    notify_observers(true, ev, current);
  }

  static void notify_device_removed(const device_event& ev, std::vector<device_event> current)
  {
    std::erase_if(current, [&](auto& d) { return d.id == ev.id; });

    // Open ports first: the application may react to the observer notification by reopening.
    struct loss
    {
      std::vector<report> reports;
      midi_error_callback on_error;
      midi_warning_callback on_warning;
    };
    std::vector<loss> losses;
    std::vector<device_event> before;
    {
      std::lock_guard l{registry_mutex};
      before = std::move(hotplug_snapshot);
      hotplug_snapshot = current;
      for (auto& [token, entry] : targets)
      {
        if (entry.device_id != ev.id)
          continue;
        loss& x = losses.emplace_back(loss{{}, entry.on_error, entry.on_warning});
        entry.t->on_device_lost(x.reports);
      }
    }
    for (const auto& x : losses)
      deliver_reports(x.reports, x.on_error, x.on_warning);

    // The device is gone from `current`: index it where it was before the removal, if known.
    if (std::none_of(before.begin(), before.end(), [&](auto& d) { return d.id == ev.id; }))
      before = {ev};
    notify_observers(false, ev, before);
  }
};

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong target_ptr, jboolean is_output);
extern "C" JNIEXPORT void JNICALL
Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpenFailed(
    JNIEnv* env, jobject /*thiz*/, jlong target_ptr, jboolean is_output);
extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_DeviceObserver_onDeviceAddedNative(
    JNIEnv* env, jobject /*thiz*/, jobject device_info);
extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_DeviceObserver_onDeviceRemovedNative(
    JNIEnv* env, jobject /*thiz*/, jobject device_info);
}
}

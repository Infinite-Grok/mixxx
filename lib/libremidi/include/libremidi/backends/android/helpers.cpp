#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/backends/android/midi_in.hpp>
#include <libremidi/backends/android/midi_out.hpp>

NAMESPACE_LIBREMIDI::android
{
// Main source of knowledge is RtMidi implementation
// which was done by Yellow Labrador

JNIEnv* context::get_thread_env()
{
  JNIEnv* env;
  JavaVM* jvm;
  jsize count;
  if (JNI_GetCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || count < 1)
  {
    LOGE("No JVM found");
    return nullptr;
  }

  if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
  {
    if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
      LOGE("Failed to attach thread");
      return nullptr;
    }
  }

  return env;
}

jobject context::get_context(JNIEnv* env)
{
  auto activity_thread = env->FindClass("android/app/ActivityThread");
  auto current_activity_thread = env->GetStaticMethodID(
      activity_thread, "currentActivityThread", "()Landroid/app/ActivityThread;");
  auto at = env->CallStaticObjectMethod(activity_thread, current_activity_thread);

  if (!at)
  {
    LOGE("Failed to get ActivityThread");
    return nullptr;
  }

  auto get_application
      = env->GetMethodID(activity_thread, "getApplication", "()Landroid/app/Application;");
  auto app = env->CallObjectMethod(at, get_application);

  if (!app)
  {
    LOGE("Failed to get Application");
    return nullptr;
  }

  return app;
}

jobject context::get_midi_manager(JNIEnv* env, jobject ctx)
{
  auto context_class = env->FindClass("android/content/Context");
  auto get_system_service = env->GetMethodID(
      context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  auto midi_service_str = env->NewStringUTF("midi");
  auto service = env->CallObjectMethod(ctx, get_system_service, midi_service_str);
  env->DeleteLocalRef(midi_service_str);
  return service;
}

void context::refresh_midi_devices(JNIEnv* env, jobject ctx, bool is_output)
{
  cleanup_devices(env);

  auto midi_service = get_midi_manager(env, ctx);
  if (!midi_service)
    return;

  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto get_devices_method
      = env->GetMethodID(midi_mgr_class, "getDevices", "()[Landroid/media/midi/MidiDeviceInfo;");
  auto device_array = (jobjectArray)env->CallObjectMethod(midi_service, get_devices_method);

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_input_count = env->GetMethodID(device_info_class, "getInputPortCount", "()I");
  auto get_output_count = env->GetMethodID(device_info_class, "getOutputPortCount", "()I");
  auto get_id = env->GetMethodID(device_info_class, "getId", "()I");

  jsize count = env->GetArrayLength(device_array);
  for (jsize i = 0; i < count; ++i)
  {
    auto device_info = env->GetObjectArrayElement(device_array, i);
    int port_count = is_output ? env->CallIntMethod(device_info, get_input_count)
                               : env->CallIntMethod(device_info, get_output_count);

    if (port_count > 0)
    {
      midi_devices.push_back(env->NewGlobalRef(device_info));
      midi_device_ids.push_back(env->CallIntMethod(device_info, get_id));
    }
    env->DeleteLocalRef(device_info);
  }

  env->DeleteLocalRef(device_array);
  env->DeleteLocalRef(midi_service);
}

// MidiDeviceInfo.getProperties().getString("name")
static std::string device_name(JNIEnv* env, jobject device_info)
{
  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method
      = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method
      = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
  auto key = env->NewStringUTF("name");
  auto name_str = (jstring)env->CallObjectMethod(bundle, get_string_method, key);

  std::string result;
  if (name_str)
  {
    const char* name_chars = env->GetStringUTFChars(name_str, nullptr);
    result = name_chars;
    env->ReleaseStringUTFChars(name_str, name_chars);
    env->DeleteLocalRef(name_str);
  }

  env->DeleteLocalRef(key);
  env->DeleteLocalRef(bundle);

  return result;
}

std::string context::port_name(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_devices.size())
  {
    LOGE("Invalid port number");
    return "";
  }

  return device_name(env, midi_devices[port_number]);
}

device_event context::read_device_event(JNIEnv* env, jobject device_info)
{
  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_id = env->GetMethodID(device_info_class, "getId", "()I");
  auto get_input_count = env->GetMethodID(device_info_class, "getInputPortCount", "()I");
  auto get_output_count = env->GetMethodID(device_info_class, "getOutputPortCount", "()I");

  device_event ev;
  ev.id = env->CallIntMethod(device_info, get_id);
  ev.input_ports = env->CallIntMethod(device_info, get_input_count);
  ev.output_ports = env->CallIntMethod(device_info, get_output_count);
  ev.name = device_name(env, device_info);
  env->DeleteLocalRef(device_info_class);
  return ev;
}

std::vector<device_event> context::list_devices(JNIEnv* env, jobject ctx)
{
  std::vector<device_event> result;

  auto midi_service = get_midi_manager(env, ctx);
  if (!midi_service)
    return result;

  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto get_devices_method
      = env->GetMethodID(midi_mgr_class, "getDevices", "()[Landroid/media/midi/MidiDeviceInfo;");
  auto device_array = (jobjectArray)env->CallObjectMethod(midi_service, get_devices_method);
  if (device_array)
  {
    jsize count = env->GetArrayLength(device_array);
    for (jsize i = 0; i < count; ++i)
    {
      auto device_info = env->GetObjectArrayElement(device_array, i);
      result.push_back(read_device_event(env, device_info));
      env->DeleteLocalRef(device_info);
    }
    env->DeleteLocalRef(device_array);
  }
  env->DeleteLocalRef(midi_service);
  return result;
}

void context::cleanup_devices(JNIEnv* env)
{
  for (jobject device : midi_devices)
  {
    env->DeleteGlobalRef(device);
  }
  midi_devices.clear();
  midi_device_ids.clear();
}

jclass context::find_app_class(JNIEnv* env, jobject ctx, const char* dotted_name)
{
  jclass result = nullptr;

  // FindClass() on a thread that was attached from native code only sees the system
  // class loader, which does not know the application's classes: go through the
  // application's own class loader first.
  auto ctx_class = env->GetObjectClass(ctx);
  auto get_loader = env->GetMethodID(ctx_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
  auto loader = get_loader ? env->CallObjectMethod(ctx, get_loader) : nullptr;
  env->DeleteLocalRef(ctx_class);
  if (env->ExceptionCheck())
    env->ExceptionClear();

  if (loader)
  {
    auto loader_class = env->FindClass("java/lang/ClassLoader");
    auto load_class
        = env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    auto name = env->NewStringUTF(dotted_name);
    result = (jclass)env->CallObjectMethod(loader, load_class, name);
    if (env->ExceptionCheck())
    {
      env->ExceptionClear();
      result = nullptr;
    }
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(loader);
  }

  if (!result)
  {
    std::string slashed = dotted_name;
    for (auto& c : slashed)
      if (c == '.')
        c = '/';
    result = env->FindClass(slashed.c_str());
    if (env->ExceptionCheck())
    {
      env->ExceptionClear();
      result = nullptr;
    }
  }
  return result;
}

bool context::open_device(jobject device_info, void* cookie, bool is_output)
{
  auto env = get_thread_env();
  if (!env)
    return false;

  auto ctx = get_context(env);
  if (!ctx)
    return false;

  auto midi_mgr = get_midi_manager(env, ctx);
  if (!midi_mgr)
    return false;

  // Check if we're on the main thread
  auto looper_class = env->FindClass("android/os/Looper");
  auto get_my_looper_method
      = env->GetStaticMethodID(looper_class, "myLooper", "()Landroid/os/Looper;");
  auto my_looper = env->CallStaticObjectMethod(looper_class, get_my_looper_method);

  jobject handler = nullptr;
  if (!my_looper)
  {
    LOGI("Not on a Looper thread, using main looper");
    auto get_main_looper_method
        = env->GetStaticMethodID(looper_class, "getMainLooper", "()Landroid/os/Looper;");
    auto main_looper = env->CallStaticObjectMethod(looper_class, get_main_looper_method);

    auto handler_class = env->FindClass("android/os/Handler");
    auto handler_ctor = env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V");
    handler = env->NewObject(handler_class, handler_ctor, main_looper);

    env->DeleteLocalRef(main_looper);
  }

  // Create the callback listener
  auto callback_class = find_app_class(env, ctx, "dev.celtera.libremidi.MidiDeviceCallback");
  if (!callback_class)
  {
    LOGE("MidiDeviceCallback class not found - ensure the Java class is part of the app");
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }

  auto callback_ctor = env->GetMethodID(callback_class, "<init>", "(JZ)V");
  auto callback = env->NewObject(
      callback_class, callback_ctor, (jlong)(intptr_t)cookie, (jboolean)is_output);
  env->DeleteLocalRef(callback_class);
  if (!callback)
  {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }

  // Open the device
  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto open_device_method = env->GetMethodID(
      midi_mgr_class, "openDevice",
      "(Landroid/media/midi/MidiDeviceInfo;Landroid/media/midi/"
      "MidiManager$OnDeviceOpenedListener;Landroid/os/Handler;)V");

  env->CallVoidMethod(midi_mgr, open_device_method, device_info, callback, handler);
  const bool ok = !env->ExceptionCheck();
  if (!ok)
    env->ExceptionClear();

  env->DeleteLocalRef(callback);
  if (handler)
    env->DeleteLocalRef(handler);
  return ok;
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong target_ptr, jboolean /*is_output*/)
{
  if (target_ptr == 0)
  {
    LOGE("Invalid target in callback");
    return;
  }

  AMidiDevice* amidi_device = nullptr;
  if (midi_device)
    AMidiDevice_fromJava(env, midi_device, &amidi_device);

  if (!amidi_device)
    LOGE("Failed to convert Java MIDI device to AMidiDevice");

  // A null device is reported to the port as an open failure.
  context::handle_device_opened(amidi_device, target_ptr);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpenFailed(
    JNIEnv* /*env*/, jobject /*thiz*/, jlong target_ptr, jboolean /*is_output*/)
{
  if (target_ptr == 0)
  {
    LOGE("Invalid target in callback");
    return;
  }
  context::handle_device_opened(nullptr, target_ptr);
}

// ---- MidiManager.DeviceCallback (Java class DeviceObserver) ----
namespace
{
std::mutex g_device_callback_mutex;
int g_device_callback_users = 0;
jobject g_device_callback = nullptr; // global refs, valid while registered
jobject g_device_callback_manager = nullptr;
}

void context::acquire_device_callback()
{
  std::lock_guard l{g_device_callback_mutex};
  g_device_callback_users++;
  if (g_device_callback)
    return;

  auto env = get_thread_env();
  auto ctx = env ? get_context(env) : nullptr;
  auto midi_mgr = ctx ? get_midi_manager(env, ctx) : nullptr;
  if (!midi_mgr)
  {
    LOGE("Cannot watch MIDI devices: no MidiManager");
    return;
  }

  auto observer_class = find_app_class(env, ctx, "dev.celtera.libremidi.DeviceObserver");
  if (!observer_class)
  {
    LOGE("DeviceObserver class not found - device removal cannot be detected, "
         "ensure the Java class is part of the app");
    return;
  }
  auto observer_ctor = env->GetMethodID(observer_class, "<init>", "()V");
  auto callback = env->NewObject(observer_class, observer_ctor);
  env->DeleteLocalRef(observer_class);
  if (!callback)
  {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    return;
  }

  // Deliver on the main looper
  auto looper_class = env->FindClass("android/os/Looper");
  auto get_main_looper_method
      = env->GetStaticMethodID(looper_class, "getMainLooper", "()Landroid/os/Looper;");
  auto main_looper = env->CallStaticObjectMethod(looper_class, get_main_looper_method);
  auto handler_class = env->FindClass("android/os/Handler");
  auto handler_ctor = env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V");
  auto handler = env->NewObject(handler_class, handler_ctor, main_looper);

  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto register_method = env->GetMethodID(
      midi_mgr_class, "registerDeviceCallback",
      "(Landroid/media/midi/MidiManager$DeviceCallback;Landroid/os/Handler;)V");
  env->CallVoidMethod(midi_mgr, register_method, callback, handler);
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    LOGE("MidiManager.registerDeviceCallback failed");
  }
  else
  {
    g_device_callback = env->NewGlobalRef(callback);
    g_device_callback_manager = env->NewGlobalRef(midi_mgr);
  }

  env->DeleteLocalRef(handler);
  env->DeleteLocalRef(main_looper);
  env->DeleteLocalRef(callback);
  env->DeleteLocalRef(midi_mgr);
}

void context::release_device_callback()
{
  std::lock_guard l{g_device_callback_mutex};
  if (g_device_callback_users > 0)
    g_device_callback_users--;
  if (g_device_callback_users > 0 || !g_device_callback)
    return;

  if (auto env = get_thread_env())
  {
    auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
    auto unregister_method = env->GetMethodID(
        midi_mgr_class, "unregisterDeviceCallback",
        "(Landroid/media/midi/MidiManager$DeviceCallback;)V");
    env->CallVoidMethod(g_device_callback_manager, unregister_method, g_device_callback);
    if (env->ExceptionCheck())
      env->ExceptionClear();
    env->DeleteGlobalRef(g_device_callback);
    env->DeleteGlobalRef(g_device_callback_manager);
    g_device_callback = nullptr;
    g_device_callback_manager = nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_DeviceObserver_onDeviceAddedNative(
    JNIEnv* env, jobject /*thiz*/, jobject device_info)
{
  auto ctx = context::get_context(env);
  if (!device_info || !ctx)
    return;
  context::notify_device_added(
      context::read_device_event(env, device_info), context::list_devices(env, ctx));
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_DeviceObserver_onDeviceRemovedNative(
    JNIEnv* env, jobject /*thiz*/, jobject device_info)
{
  auto ctx = context::get_context(env);
  if (!device_info || !ctx)
    return;
  context::notify_device_removed(
      context::read_device_event(env, device_info), context::list_devices(env, ctx));
}

}

#pragma once
#include <jni.h>

#include <libremidi/config.hpp>

NAMESPACE_LIBREMIDI::android::detail
{
//! Detaches the calling thread from the JVM when the thread exits, but only for a thread that
//! this library attached itself.
//!
//! ART aborts the process when a native thread that was attached with AttachCurrentThread exits
//! without DetachCurrentThread ("Native thread exited without calling DetachCurrentThread").
//! Detaching also frees the local references the thread accumulated.
struct jni_thread_detacher
{
  JavaVM* jvm{};
  ~jni_thread_detacher()
  {
    if (jvm)
      jvm->DetachCurrentThread();
  }
};

//! JNIEnv of the calling thread. A thread that is already attached (Java threads, the main
//! looper, a thread attached by Qt's QJniEnvironment or by the application) is used as is and is
//! never detached here. Any other thread is attached, and detached again when it exits.
//! Returns nullptr on failure.
inline JNIEnv* get_or_attach_thread_env(JavaVM* jvm)
{
  JNIEnv* env = nullptr;
  const jint rc = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (rc == JNI_OK)
    return env;
  if (rc != JNI_EDETACHED)
    return nullptr;

  if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    return nullptr;

  // Constructed on the first attach of this thread: its destructor runs at thread exit.
  thread_local jni_thread_detacher detacher;
  detacher.jvm = jvm;
  return env;
}
}

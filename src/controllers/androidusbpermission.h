#pragma once

#include <functional>

namespace mixxx {
namespace android {

/// Called on the Android UI thread (JNI) for every USB permission result, with the device's vendor/product id.
/// There is one listener; the sound manager uses it to reapply the sound configuration after a grant.
/// setUsbPermissionListener() blocks until a running notification has finished, so clearing it
/// (empty function) before destroying the owner is safe.
using UsbPermissionListener = std::function<void(int vendorId, int productId, bool granted)>;
void setUsbPermissionListener(UsbPermissionListener listener);

} // namespace android
} // namespace mixxx

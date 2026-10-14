#ifndef USBDIRECT_DEVICE_PROFILE_H
#define USBDIRECT_DEVICE_PROFILE_H
/* Registry of supported devices. A profile = match rule (VID/PID + exact raw descriptor bytes)
 * + StreamFormat + transport variant + quirk flags. Unknown devices match nothing. */
#include <cstddef>
#include <cstdint>

#include "stream_format.h"

namespace usbdirect {

enum class TransportVariant { AsyncExplicitFeedbackFS = 0 };

/* Quirk flags (none defined yet). */
constexpr uint32_t kNoQuirks = 0;

struct DeviceProfile {
    uint16_t vendorId;
    uint16_t productId;
    const unsigned char *descriptor; /* exact raw descriptors required */
    size_t descriptorLen;
    StreamFormat format;
    TransportVariant transport;
    uint32_t quirks;
};

/* The 288-byte T7 descriptor capture from probe G1 (identical to the I1 profile.h). */
constexpr size_t kT7DescriptorLen = 288;
extern const unsigned char kT7Descriptor[kT7DescriptorLen];

/* Returns the profile whose VID/PID (read from the device descriptor at raw[8..11]) and exact
 * raw bytes match, else nullptr. Touches no USB. */
const DeviceProfile *findProfile(const unsigned char *raw, size_t len);
/* The registry table (for enumeration in tests/UI). */
const DeviceProfile *profiles(size_t *count);

} // namespace usbdirect
#endif

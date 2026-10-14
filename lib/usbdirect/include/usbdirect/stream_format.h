#ifndef USBDIRECT_STREAM_FORMAT_H
#define USBDIRECT_STREAM_FORMAT_H
/* Controller-agnostic description of the PCM stream a device accepts, and the sample packer. */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace usbdirect {

enum class SampleFormat { S16LE = 0, S24_3LE = 1, S32LE = 2 };
enum class ChannelRole { Unknown = 0, MainL, MainR, PhonesL, PhonesR };

inline unsigned bytesPerSample(SampleFormat f) {
    return f == SampleFormat::S16LE ? 2u : f == SampleFormat::S24_3LE ? 3u : 4u;
}

constexpr unsigned kMaxChannels = 8;

struct StreamFormat {
    unsigned sampleRate = 0; /* Hz; 0 = no device open */
    unsigned channels = 0;
    SampleFormat sampleFormat = SampleFormat::S24_3LE;
    unsigned bytesPerFrame = 0; /* channels * bytesPerSample(sampleFormat) */
    ChannelRole channelRoles[kMaxChannels] = {};
    std::string profileId;
    std::string displayName;

    bool valid() const { return sampleRate != 0 && channels != 0 && bytesPerFrame != 0; }
};

/* Builds a StreamFormat with bytesPerFrame derived and roles copied (roles may be shorter than
 * channels; the rest stay Unknown). */
inline StreamFormat makeFormat(unsigned rate, unsigned channels, SampleFormat f, const ChannelRole *roles,
                               size_t nRoles, const char *id, const char *name) {
    StreamFormat s;
    s.sampleRate = rate;
    s.channels = channels > kMaxChannels ? kMaxChannels : channels;
    s.sampleFormat = f;
    s.bytesPerFrame = s.channels * bytesPerSample(f);
    for (size_t i = 0; i < nRoles && i < s.channels; i++) s.channelRoles[i] = roles[i];
    s.profileId = id;
    s.displayName = name;
    return s;
}

/* One sample, clamped to [-1,1], asymmetric full scale (-1 -> min, +1 -> max); little endian.
 * S24_3LE is the exact I1/probe packing (t7_pack24). Allocation-free. */
inline void packSample(unsigned char *b, double x, SampleFormat f) {
    if (x != x) x = 0; /* NaN -> silence, explicit (llround(NaN) is unspecified) */
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    switch (f) {
    case SampleFormat::S16LE: {
        const int32_t s = (int32_t)std::llround(x * (x < 0 ? 32768.0 : 32767.0));
        b[0] = (unsigned char)s;
        b[1] = (unsigned char)(s >> 8);
        break;
    }
    case SampleFormat::S24_3LE: {
        const int32_t s = (int32_t)std::llround(x * (x < 0 ? 8388608.0 : 8388607.0));
        b[0] = (unsigned char)s;
        b[1] = (unsigned char)(s >> 8);
        b[2] = (unsigned char)(s >> 16);
        break;
    }
    case SampleFormat::S32LE: {
        const int64_t s = (int64_t)std::llround(x * (x < 0 ? 2147483648.0 : 2147483647.0));
        for (int i = 0; i < 4; i++) b[i] = (unsigned char)(s >> (8 * i));
        break;
    }
    }
}

/* Packs `frames` interleaved float frames of `channels` channels. */
inline void packFrames(unsigned char *out, const float *in, unsigned frames, unsigned channels, SampleFormat f) {
    const unsigned bps = bytesPerSample(f);
    for (size_t i = 0; i < (size_t)frames * channels; i++) packSample(out + i * bps, (double)in[i], f);
}

} // namespace usbdirect
#endif

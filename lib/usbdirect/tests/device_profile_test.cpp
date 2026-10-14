/* Registry matching and the generalised sample packer (unit level). */
#include <cstring>
#include <vector>

#include "check.h"
#include "usbdirect/device_profile.h"
#include "usbdirect/stream_format.h"
#include "usbdirect/transport_core.h"

using namespace usbdirect;

static void testRegistry() {
    size_t n = 0;
    const DeviceProfile *all = profiles(&n);
    CHECK(n == 1 && all);
    const DeviceProfile *t7 = findProfile(kT7Descriptor, kT7DescriptorLen);
    CHECK(t7 == &all[0]);
    CHECK(t7->vendorId == 0x06f8 && t7->productId == 0xb12e);
    CHECK(t7->transport == TransportVariant::AsyncExplicitFeedbackFS && t7->quirks == kNoQuirks);
    const StreamFormat &f = t7->format;
    CHECK(f.valid() && f.sampleRate == 44100 && f.channels == 4 && f.sampleFormat == SampleFormat::S24_3LE);
    CHECK(f.bytesPerFrame == 12);
    CHECK(f.channelRoles[0] == ChannelRole::MainL && f.channelRoles[1] == ChannelRole::MainR);
    CHECK(f.channelRoles[2] == ChannelRole::PhonesL && f.channelRoles[3] == ChannelRole::PhonesR);
    CHECK(f.profileId == "hercules-djcontrol-inpulse-t7" && !f.displayName.empty());
    CHECK(t7->descriptorLen == 288 && kT7DescriptorLen == 288);
    /* the device descriptor's own VID/PID agree with the rule */
    CHECK(kT7Descriptor[8] == 0xf8 && kT7Descriptor[9] == 0x06 && kT7Descriptor[10] == 0x2e && kT7Descriptor[11] == 0xb1);

    /* every single-byte change, anywhere, rejects (includes VID/PID bytes) */
    std::vector<unsigned char> b(kT7Descriptor, kT7Descriptor + kT7DescriptorLen);
    for (size_t i = 0; i < b.size(); i++) {
        b[i] ^= 0x01;
        CHECK(findProfile(b.data(), b.size()) == nullptr);
        b[i] ^= 0x01;
    }
    CHECK(findProfile(b.data(), b.size()) == t7);
    /* VID or PID mismatch alone */
    b[8] ^= 0x10;
    CHECK(findProfile(b.data(), b.size()) == nullptr);
    b[8] ^= 0x10;
    b[11] ^= 0x10;
    CHECK(findProfile(b.data(), b.size()) == nullptr);
    b[11] ^= 0x10;
    /* length / null / short */
    CHECK(findProfile(b.data(), b.size() - 1) == nullptr);
    std::vector<unsigned char> longer(b);
    longer.push_back(0);
    CHECK(findProfile(longer.data(), longer.size()) == nullptr);
    CHECK(findProfile(nullptr, 288) == nullptr);
    CHECK(findProfile(b.data(), 0) == nullptr);
    CHECK(findProfile(b.data(), 17) == nullptr);
    std::vector<unsigned char> zeros(300, 0);
    CHECK(findProfile(zeros.data(), zeros.size()) == nullptr);
}

/* T7 packing must be byte-identical to the I1 packer (t7_pack24) for every sample. */
static void testT7PackIdentity() {
    const unsigned frames = 45, ch = 4;
    std::vector<float> in(frames * ch);
    uint32_t seed = 12345;
    for (int round = 0; round < 2000; round++) {
        for (auto &x : in) {
            seed = seed * 1664525u + 1013904223u;
            x = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 1.2f; /* includes out-of-range values */
        }
        in[0] = 1.0f; in[1] = -1.0f; in[2] = 0.0f; in[3] = 1.5f; in[4] = -1.5f;
        std::vector<unsigned char> a(frames * 12), ref(frames * 12);
        packFrames(a.data(), in.data(), frames, ch, SampleFormat::S24_3LE);
        for (unsigned i = 0; i < frames * ch; i++) t7_pack24(ref.data() + i * 3, (double)in[i]);
        CHECK(a == ref);
    }
}

static int32_t rd(const unsigned char *b, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint32_t)b[i] << (8 * i);
    if (n < 4 && (b[n - 1] & 0x80)) v |= ~0u << (8 * n);
    return (int32_t)v;
}

static void testOtherFormats() {
    CHECK(bytesPerSample(SampleFormat::S16LE) == 2 && bytesPerSample(SampleFormat::S24_3LE) == 3 &&
          bytesPerSample(SampleFormat::S32LE) == 4);
    { /* 2-channel S16 */
        const float in[2 * 3] = {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -2.0f};
        unsigned char out[2 * 3 * 2];
        packFrames(out, in, 3, 2, SampleFormat::S16LE);
        CHECK(rd(out + 0, 2) == 0 && rd(out + 2, 2) == 32767 && rd(out + 4, 2) == -32768);
        CHECK(rd(out + 6, 2) == (int32_t)std::llround(0.5 * 32767.0));
        CHECK(rd(out + 8, 2) == 32767 && rd(out + 10, 2) == -32768); /* clamped */
        CHECK(out[2] == 0xff && out[3] == 0x7f && out[4] == 0x00 && out[5] == 0x80); /* little endian */
        const StreamFormat s = makeFormat(48000, 2, SampleFormat::S16LE, nullptr, 0, "synthetic-2ch-s16", "S16");
        CHECK(s.bytesPerFrame == 4 && s.channelRoles[0] == ChannelRole::Unknown && s.valid());
    }
    { /* 4-channel S32 */
        const float in[4 * 2] = {0.0f, 1.0f, -1.0f, 0.25f, -0.25f, 3.0f, -3.0f, 0.125f};
        unsigned char out[4 * 2 * 4];
        packFrames(out, in, 2, 4, SampleFormat::S32LE);
        CHECK(rd(out + 0, 4) == 0 && rd(out + 4, 4) == 2147483647 && rd(out + 8, 4) == INT32_MIN);
        CHECK(rd(out + 12, 4) == (int32_t)std::llround(0.25 * 2147483647.0));
        CHECK(rd(out + 16, 4) == (int32_t)std::llround(-0.25 * 2147483648.0));
        CHECK(rd(out + 20, 4) == 2147483647 && rd(out + 24, 4) == INT32_MIN);
        CHECK(rd(out + 28, 4) == (int32_t)std::llround(0.125 * 2147483647.0));
        const ChannelRole roles[2] = {ChannelRole::MainL, ChannelRole::MainR};
        const StreamFormat s = makeFormat(96000, 4, SampleFormat::S32LE, roles, 2, "synthetic-4ch-s32", "S32");
        CHECK(s.bytesPerFrame == 16 && s.channelRoles[1] == ChannelRole::MainR && s.channelRoles[2] == ChannelRole::Unknown);
    }
    { /* 24-bit extremes match the documented asymmetric scale */
        const float in[3] = {1.0f, -1.0f, 0.0f};
        unsigned char out[9];
        packFrames(out, in, 3, 1, SampleFormat::S24_3LE);
        CHECK(rd(out, 3) == 8388607 && rd(out + 3, 3) == -8388608 && rd(out + 6, 3) == 0);
    }
    CHECK(!StreamFormat().valid());
}

/* NaN input packs as silence in every format (explicit, not llround(NaN)); +-inf clamp. */
static void testNanAndInf() {
    const double nan = std::nan("");
    const double inf = INFINITY;
    for (SampleFormat f : {SampleFormat::S16LE, SampleFormat::S24_3LE, SampleFormat::S32LE}) {
        unsigned char b[4] = {0xaa, 0xaa, 0xaa, 0xaa};
        packSample(b, nan, f);
        for (unsigned i = 0; i < bytesPerSample(f); i++) CHECK(b[i] == 0);
        packSample(b, -nan, f);
        for (unsigned i = 0; i < bytesPerSample(f); i++) CHECK(b[i] == 0);
        unsigned char p[4], n[4];
        packSample(p, inf, f);
        packSample(n, -inf, f);
        CHECK(rd(p, bytesPerSample(f)) > 0 && rd(n, bytesPerSample(f)) < 0);
    }
}

int main() {
    testNanAndInf();
    testRegistry();
    testT7PackIdentity();
    testOtherFormats();
    PASS("device_profile");
}

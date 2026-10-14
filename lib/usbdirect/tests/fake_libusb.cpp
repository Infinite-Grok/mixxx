#include "fake_libusb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace fake {

Config cfg;
std::vector<std::string> calls;
std::vector<libusb_transfer *> pending;
std::vector<Sub> subs;
int allocs = 0, frees = 0, submits = 0, event_calls = 0;
bool in_callback = false, in_fake = false;
void (*events_hook)() = nullptr;
static std::set<libusb_transfer *> cancelled;
static int dummy_ctx, dummy_dev, dummy_handle;

void reset() {
    in_fake = true;
    cfg = Config();
    calls.clear();
    calls.reserve(256);
    pending.clear();
    pending.reserve(64);
    subs.clear();
    subs.reserve(64);
    cancelled.clear();
    allocs = frees = submits = event_calls = 0;
    in_callback = false;
    events_hook = nullptr;
    in_fake = false;
}

static void log(const char *s) {
    in_fake = true;
    calls.emplace_back(s);
    in_fake = false;
}
bool has(const std::string &c) { return std::find(calls.begin(), calls.end(), c) != calls.end(); }
int index_of(const std::string &c, int from) {
    for (size_t i = (size_t)from; i < calls.size(); i++)
        if (calls[i] == c) return (int)i;
    return -1;
}
int pending_on(unsigned char ep) {
    int n = 0;
    for (auto *t : pending) n += t->endpoint == ep;
    return n;
}

void complete(libusb_transfer *t, int status, int pkt_status, int actual) {
    in_fake = true;
    auto it = std::find(pending.begin(), pending.end(), t);
    if (it == pending.end()) {
        std::fprintf(stderr, "fake: completing a transfer that is not pending\n");
        std::abort();
    }
    pending.erase(it);
    cancelled.erase(t);
    in_fake = false;
    t->status = (libusb_transfer_status)status;
    t->iso_packet_desc[0].status = (libusb_transfer_status)pkt_status;
    t->iso_packet_desc[0].actual_length = (unsigned)actual;
    in_callback = true;
    t->callback(t);
    in_callback = false;
}

} // namespace fake

using namespace fake;

extern "C" {

int LIBUSB_CALL libusb_init_context(libusb_context **ctx, const struct libusb_init_option[], int) {
    log("init");
    if (cfg.init_rc) return cfg.init_rc;
    *ctx = (libusb_context *)&dummy_ctx;
    return 0;
}
void LIBUSB_CALL libusb_exit(libusb_context *) { log("exit"); }
int LIBUSB_CALL libusb_wrap_sys_device(libusb_context *, intptr_t, libusb_device_handle **h) {
    log("wrap");
    if (cfg.wrap_rc) return cfg.wrap_rc;
    *h = (libusb_device_handle *)&dummy_handle;
    return 0;
}
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *) { return (libusb_device *)&dummy_dev; }
int LIBUSB_CALL libusb_get_device_speed(libusb_device *) {
    log("speed");
    return cfg.speed;
}
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *, int i) {
    static const char *const n[] = {"driver0", "driver1", "driver2", "driver3"};
    log(n[i]);
    return cfg.driver[i];
}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *, int i) {
    log(i == 1 ? "claim1" : "claim?");
    return cfg.claim_rc[cfg.claim_calls < 4 ? cfg.claim_calls++ : 3];
}
int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *, int i) {
    log(i == 1 ? "detach1" : "detach?");
    return cfg.detach_rc;
}
int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle *, int i) {
    log(i == 1 ? "attach1" : "attach?");
    return 0;
}
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *, int i, int alt) {
    log(i == 1 && alt == 1 ? "alt1=1" : i == 1 && alt == 0 ? "alt1=0" : "alt?");
    return alt == 1 ? cfg.alt_rc : 0;
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *, int i) {
    log(i == 1 ? "release1" : "release?");
    return 0;
}
void LIBUSB_CALL libusb_close(libusb_device_handle *) { log("close"); }

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets) {
    allocs++;
    return (libusb_transfer *)calloc(1, sizeof(libusb_transfer) + (size_t)iso_packets * sizeof(libusb_iso_packet_descriptor));
}
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t) {
    if (std::find(pending.begin(), pending.end(), t) != pending.end()) {
        std::fprintf(stderr, "fake: freeing a transfer that is still pending (reap-before-free violated)\n");
        std::abort();
    }
    log("free");
    frees++;
    free(t);
}
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t) {
    submits++;
    if (cfg.fail_submit_at && submits == cfg.fail_submit_at) return LIBUSB_ERROR_NO_DEVICE;
    in_fake = true;
    pending.push_back(t);
    if (subs.size() < subs.capacity()) {
        Sub s;
        s.ep = t->endpoint;
        s.length = t->length;
        std::memcpy(s.data, t->buffer, (size_t)t->length > sizeof(s.data) ? sizeof(s.data) : (size_t)t->length);
        subs.push_back(s);
    }
    in_fake = false;
    return 0;
}
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t) {
    log("cancel");
    in_fake = true;
    bool found = std::find(pending.begin(), pending.end(), t) != pending.end();
    bool fresh = found && cancelled.insert(t).second;
    in_fake = false;
    return fresh ? 0 : LIBUSB_ERROR_NOT_FOUND;
}
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *, struct timeval *) {
    event_calls++;
    if (events_hook) events_hook();
    if (cfg.hold_events > 0) {
        cfg.hold_events--;
        return 0;
    }
    std::vector<libusb_transfer *> todo;
    in_fake = true;
    for (auto *t : pending)
        if (cancelled.count(t)) todo.push_back(t);
    in_fake = false;
    for (auto *t : todo) complete(t, LIBUSB_TRANSFER_CANCELLED, LIBUSB_TRANSFER_CANCELLED, 0);
    return 0;
}

} // extern "C"

#ifndef USBDIRECT_FAKE_LIBUSB_H
#define USBDIRECT_FAKE_LIBUSB_H
/* Link-time fake of the libusb entry points used by usbdirect. Transfers are queued on submit and
 * completed by the test (fake::complete) the way libusb would from handle_events. Types and
 * inline helpers (libusb_fill_iso_transfer, ...) come from the real libusb.h. */
#include <libusb.h>

#include <string>
#include <vector>

namespace fake {

struct Sub {
    unsigned char ep;
    int length;
    unsigned char data[540];
};

struct Config {
    int init_rc = 0, wrap_rc = 0, speed = LIBUSB_SPEED_FULL;
    int driver[4] = {0, 0, 0, 0};
    int claim_rc[4] = {0, 0, 0, 0}; /* results of successive claim calls */
    int claim_calls = 0;
    int detach_rc = 0, alt_rc = 0;
    int fail_submit_at = 0;          /* 1-based submit call number to fail with NO_DEVICE */
    int hold_events = 0;             /* handle_events calls that deliver nothing */
};

extern Config cfg;
extern std::vector<std::string> calls;       /* low-rate call log (not submit/handle_events) */
extern std::vector<libusb_transfer *> pending; /* submitted, not yet completed (submission order) */
extern std::vector<Sub> subs;                /* submissions since last clear (reserved: no growth) */
extern int allocs, frees, submits, event_calls;
extern bool in_callback, in_fake;            /* in_fake: fake bookkeeping, excluded from alloc tests */
extern void (*events_hook)();

void reset();
void complete(libusb_transfer *t, int status, int pkt_status, int actual);
/* Count pending transfers on an endpoint. */
int pending_on(unsigned char ep);
bool has(const std::string &call);
int index_of(const std::string &call, int from = 0);

} // namespace fake
#endif

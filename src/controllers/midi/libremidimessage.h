#pragma once

#include <cstddef>
#include <span>

/// The three bytes of a MIDI short message as expected by
/// MidiController::receivedShortMessage().
struct LibremidiShortMessage {
    unsigned char status;
    unsigned char byte1;
    unsigned char byte2;
};

/// Splits a raw (non-SysEx) MIDI message into status and up to two data bytes.
///
/// MIDI messages are 1 to 3 bytes long (e.g. program change and channel
/// pressure have a single data byte), so a missing data byte is reported as 0
/// instead of reading past the end of `bytes`. Returns false for an empty
/// message, which has no status byte.
inline bool libremidiUnpackShortMessage(
        std::span<const unsigned char> bytes, LibremidiShortMessage* pOut) {
    if (bytes.empty()) {
        return false;
    }
    pOut->status = bytes[0];
    pOut->byte1 = bytes.size() > 1 ? bytes[1] : 0;
    pOut->byte2 = bytes.size() > 2 ? bytes[2] : 0;
    return true;
}

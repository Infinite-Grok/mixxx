package dev.celtera.libremidi;

import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;

/**
 * Forwards MidiManager device events to libremidi: removal of a device that a port was opened on,
 * and the input_added / input_removed / output_added / output_removed observer callbacks.
 * libremidi creates and registers this class itself (through the application class loader), so
 * it only has to be part of the app, like MidiDeviceCallback.
 */
public class DeviceObserver extends MidiManager.DeviceCallback {
    @Override
    public void onDeviceAdded(MidiDeviceInfo device) {
        onDeviceAddedNative(device);
    }

    @Override
    public void onDeviceRemoved(MidiDeviceInfo device) {
        onDeviceRemovedNative(device);
    }

    // Native method declarations (implemented in libremidi's android/helpers.cpp)
    private native void onDeviceAddedNative(MidiDeviceInfo device);
    private native void onDeviceRemovedNative(MidiDeviceInfo device);
}

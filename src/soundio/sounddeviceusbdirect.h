#pragma once

// Android only. "USB Direct" sound device: streams the engine output straight to a USB audio
// class device (registry profile, today the Hercules DJControl Inpulse T7) over isochronous
// transfers via lib/usbdirect, bypassing Android's audio stack. See USB-DIRECT.md.

#include <QJniObject>
#include <QString>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "control/pollingcontrolproxy.h"
#include "soundio/sounddevice.h"
#include "soundio/soundmanagerconfig.h"
#include "usbdirect/device_profile.h"
#include "usbdirect/engine_pump.h"
#include "usbdirect/usb_audio_out.h"
#include "util/duration.h"
#include "util/performancetimer.h"

class SoundDeviceUsbDirect : public SoundDevice {
  public:
    // Device indexes of this API start here, so they never clash with PortAudio indexes.
    static constexpr unsigned int kDeviceIndexBase = 1000;

    SoundDeviceUsbDirect(UserSettingsPointer config,
            SoundManager* sm,
            const usbdirect::DeviceProfile* pProfile,
            unsigned int index);
    ~SoundDeviceUsbDirect() override;

    // Profiles of the attached USB devices whose VID/PID match the registry.
    static std::vector<const usbdirect::DeviceProfile*> attachedProfiles();

    SoundDeviceStatus open(bool isClkRefDevice, int syncBuffers) override;
    bool isOpen() const override;
    // Shutdown order is enforced here: UsbAudioOut::stop() returns fully, then the engine pump
    // stops, then the Java connection is closed. Call from the main thread only, never from the
    // render or low-water callbacks.
    SoundDeviceStatus close() override;
    void readProcess(SINT) override {
    }
    void writeProcess(SINT) override {
    }
    QString getError() const override {
        return m_lastError;
    }
    mixxx::audio::SampleRate getDefaultSampleRate() const override;

    const usbdirect::DeviceProfile* profile() const {
        return m_pProfile;
    }
    // Default Main / Headphones channel bases derived from the profile channelRoles; -1 if absent.
    int mainBase() const;
    int phonesBase() const;

  private:
    struct Shared; // liveness token for notifications queued to the main thread
    void render(float* interleaved, int frames);
    void onHalted();
    // Ordered shutdown (transport, then pump, then Java connection); idempotent.
    void teardown();
    bool acquireConnection(std::vector<unsigned char>* pRaw, int* pFd);

    const usbdirect::DeviceProfile* m_pProfile;
    std::shared_ptr<Shared> m_shared;

    QJniObject m_connection; // Java UsbDeviceConnection; kept alive until the transport is stopped
    std::unique_ptr<usbdirect::UsbAudioOut> m_pOut;
    std::unique_ptr<usbdirect::EnginePump> m_pPump;

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_halted{false};
    std::atomic<bool> m_haltNotified{false};
    std::atomic<bool> m_renderInit{false};
    std::thread::id m_openerThread;
    bool m_isClkRef;
    unsigned int m_frameSize;
    uint64_t m_lastEngineUnderruns;
    uint64_t m_lastRenderOverruns;
    PollingControlProxy m_audioLatencyUsage;
    mixxx::Duration m_timeInCallback;
    int m_framesSinceUsageUpdate;
    PerformanceTimer m_clkRefTimer;
    QString m_lastError;
};

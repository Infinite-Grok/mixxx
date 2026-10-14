#pragma once

#include <portaudio.h>

#include <QList>
#include <mutex>

#include "preferences/usersettings.h"
#include "soundio/sounddevice.h"
#include "soundio/sounddeviceenumerator.h"
#include "soundio/sounddeviceportaudio.h"
#ifdef Q_OS_ANDROID
#include "soundio/sounddeviceusbdirect.h"
#endif

class PortAudioEnumerator : public SoundDeviceEnumerator {
  public:
    PortAudioEnumerator(UserSettingsPointer config,
            SoundManager* sm);
    ~PortAudioEnumerator() override;

    std::vector<SoundDevicePointer> queryDevices() const override;

    QList<QString> getAPIs() const override;
    void refreshDevices(const QString& api) const override;

    QList<mixxx::audio::SampleRate> getSampleRates(bool jackSampleRate) const override;

    void initialize() override;
    void deinitialize() override;

  private:
    void setJACKName() const;

    std::vector<QSharedPointer<SoundDevicePortAudio>> m_devices;
    mixxx::audio::SampleRate m_jackSampleRate;

    UserSettingsPointer m_pConfig;
    SoundManager* m_pSoundManager;
    QList<QString> m_apis;

#ifdef Q_OS_ANDROID
    // USB Direct devices (Android): attached USB devices matching a usbdirect registry profile.
    // Guarded because queryDevices() is also called from the audio thread.
    void refreshUsbDirect() const;
    mutable std::mutex m_usbMutex;
    mutable std::vector<QSharedPointer<SoundDeviceUsbDirect>> m_usbDevices;
#endif
};

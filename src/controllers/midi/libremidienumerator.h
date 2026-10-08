#pragma once

#include <QTimer>
#include <libremidi/libremidi.hpp>

#include "controllers/controllermanager.h"
#include "controllers/midi/libremidicontroller.h"
#include "controllers/midi/midienumerator.h"
#include "preferences/usersettings.h"

/// This class handles discovery and enumeration of DJ controllers that appear
/// under the Libremidi cross-platform API.
class LibremidiEnumerator : public MidiEnumerator {
    Q_OBJECT
  public:
    LibremidiEnumerator(UserSettingsPointer pConfig, ControllerManager* pControllerManager);

    QList<Controller*> queryDevices() override;

  private:
    Controller* addDevice(const libremidi::input_port* pInputPort,
            const libremidi::output_port* pOutputPort);
    Controller* addInput(const libremidi::input_port& port);
    Controller* addOutput(const libremidi::output_port& port);
    void removeInput(const libremidi::input_port& port);
    void removeOutput(const libremidi::output_port& port);

    void removeDevice(std::vector<std::unique_ptr<LibremidiController>>::iterator it);

    /// Hotplug: a device's input and output ports are announced as separate
    /// events. Wait until the events have settled, then add or set up each
    /// touched device once, with all the ports that are known by then.
    void scheduleSetUp(LibremidiController* pDevice, bool isNew);
    void flushScheduled();
    /// Drops pDevice from the scheduled lists, returns true if it was a new
    /// device that was not yet handed to the ControllerManager.
    bool forgetScheduled(LibremidiController* pDevice);

    libremidi::observer m_observer;
    std::vector<std::unique_ptr<LibremidiController>> m_devices;
    UserSettingsPointer m_pConfig;
    ControllerManager* m_pControllerManager;
    // True while queryDevices() builds the initial device list.
    bool m_enumerating = false;
    // True once queryDevices() has taken the initial snapshot. Hotplug events
    // that arrive earlier are covered by that snapshot.
    bool m_queried = false;
    QTimer m_settleTimer;
    // Hotplugged devices not yet added to the ControllerManager.
    std::vector<LibremidiController*> m_scheduledNew;
    // Known devices that gained a port and must be set up again.
    std::vector<LibremidiController*> m_scheduledSetUp;
};

// For testing.
bool libremidiShouldLinkInputToOutput(const QString& input_name,
        const QString& output_name);

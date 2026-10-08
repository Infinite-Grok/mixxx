#include "controllers/midi/libremidienumerator.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <algorithm>
#include <libremidi/libremidi.hpp>
#include <vector>

#include "controllers/controllermanager.h"
#include "controllers/defs_controllers.h"
#include "controllers/midi/libremidicontroller.h"
#include "moc_libremidienumerator.cpp"
#include "util/cmdlineargs.h"

namespace {

bool recognizeDevice(const libremidi::port_information& deviceInfo, UserSettingsPointer pConfig) {
    // In developer mode we show the MIDI Through Port, otherwise ignore it
    // since it routinely causes trouble.
    return CmdlineArgs::Instance().getDeveloper() ||
            pConfig->getValue(kMidiThroughCfgKey, false) ||
            !QLatin1String(deviceInfo.port_name)
                     .contains(kMidiThroughPortPrefix, Qt::CaseInsensitive);
}

// Some platforms format MIDI device names as "deviceName MIDI ###" where
// ### is the instance # of the device. Therefore we want to link two
// devices that have an equivalent "deviceName" and ### section.
const QRegularExpression kMidiDeviceNameRegex(QStringLiteral("^(.*) MIDI (\\d+)( .*)?$"));

const QRegularExpression kInputRegex(QStringLiteral("^(.*) in( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kOutputRegex(QStringLiteral("^(.*) out( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);

// This is a broad pattern that matches a text blob followed by a numeral
// potentially followed by non-numeric text. The non-numeric requirement is
// meant to avoid corner cases around devices with names like "Hercules RMX
// 2" where we would potentially confuse the number in the device name as
// the ordinal index of the device.
const QRegularExpression kDeviceNameRegex(QStringLiteral("^(.*) (\\d+)( [^0-9]+)?$"));

bool namesMatchRegexes(const QRegularExpression& kInputRegex,
        const QString& input_name,
        const QRegularExpression& kOutputRegex,
        const QString& output_name) {
    QRegularExpressionMatch inputMatch = kInputRegex.match(input_name);
    if (inputMatch.hasMatch()) {
        QString inputDeviceName = inputMatch.captured(1);
        QString inputDeviceIndex = inputMatch.captured(2);
        QRegularExpressionMatch outputMatch = kOutputRegex.match(output_name);
        if (outputMatch.hasMatch()) {
            QString outputDeviceName = outputMatch.captured(1);
            QString outputDeviceIndex = outputMatch.captured(2);
            if (outputDeviceName.compare(inputDeviceName, Qt::CaseInsensitive) == 0 &&
                    outputDeviceIndex == inputDeviceIndex) {
                return true;
            }
        }
    }
    return false;
}

bool namesMatchMidiPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kMidiDeviceNameRegex, input_name, kMidiDeviceNameRegex, output_name);
}

bool namesMatchInOutPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kInputRegex, input_name, kOutputRegex, output_name);
}

bool namesMatchPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kDeviceNameRegex, input_name, kDeviceNameRegex, output_name);
}

bool namesMatchAllowableEdgeCases(const QString& input_name,
        const QString& output_name) {
    // Mac OS 10.12 & Korg Kaoss DJ 1.6:
    // Korg Kaoss DJ has input 'KAOSS DJ CONTROL' and output 'KAOSS DJ SOUND'.
    // This means it doesn't pass the libremidiShouldLinkInputToOutput test. Without an
    // output linked, the MIDI output for the device fails, as the device is
    // NULL in LibremidiController
    if (input_name == "KAOSS DJ CONTROL" && output_name == "KAOSS DJ SOUND") {
        return true;
    }
    // Ableton Push on Windows
    // Shows 2 different devices for MIDI input and output.
    if (input_name == "MIDIIN2 (Ableton Push)" && output_name == "MIDIOUT2 (Ableton Push)") {
        return true;
    }

    // Novation Launchpad X (macOS)
    if (input_name == "Launchpad X LPX DAW Out" && output_name == "Launchpad X LPX DAW In") {
        return true;
    }

    return false;
}

bool shouldLinkInputToOutput(const QString& input_name,
        const QString& output_name) {
    // Early exit.
    if (input_name == output_name || namesMatchAllowableEdgeCases(input_name, output_name)) {
        return true;
    }

    // Some device drivers prepend "To" and "From" to the names of their MIDI
    // ports. If the output and input device names don't match, let's try
    // trimming those words from the start, and seeing if they then match.

    // Ignore "From" text in the beginning of device input name.
    QString input_name_stripped = input_name;
    if (input_name.indexOf("from", 0, Qt::CaseInsensitive) == 0) {
        input_name_stripped = input_name.right(input_name.length() - 4);
    } else if (input_name.endsWith("out", Qt::CaseInsensitive)) {
        input_name_stripped = input_name.left(input_name.length() - 3);
    }

    // Ignore "To" text in the beginning of device output name.
    QString output_name_stripped = output_name;
    if (output_name.indexOf("to", 0, Qt::CaseInsensitive) == 0) {
        output_name_stripped = output_name.right(output_name.length() - 2);
    } else if (output_name.endsWith("in", Qt::CaseInsensitive)) {
        output_name_stripped = output_name.left(output_name.length() - 2);
    }

    if (output_name_stripped != input_name_stripped) {
        // Ignore " input " text in the device names
        int offset = input_name_stripped.indexOf(" input ", 0, Qt::CaseInsensitive);
        if (offset != -1) {
            input_name_stripped = input_name_stripped.replace(offset, 7, " ");
        }

        // Ignore " output " text in the device names
        offset = output_name_stripped.indexOf(" output ", 0, Qt::CaseInsensitive);
        if (offset != -1) {
            output_name_stripped = output_name_stripped.replace(offset, 8, " ");
        }
    }

    if (output_name_stripped != input_name_stripped) {
        // libremidi JACK backend appends (capture) & (playback) to its devices
        int offset = input_name_stripped.indexOf("capture", 0, Qt::CaseInsensitive);
        if (offset != -1) {
            input_name_stripped = input_name_stripped.replace(offset, 7, " ");
        }

        offset = output_name_stripped.indexOf("playback", 0, Qt::CaseInsensitive);
        if (offset != -1) {
            output_name_stripped = output_name_stripped.replace(offset, 8, " ");
        }
    }

    if (input_name_stripped == output_name_stripped ||
            namesMatchMidiPattern(input_name_stripped, output_name_stripped) ||
            namesMatchMidiPattern(input_name, output_name) ||
            namesMatchInOutPattern(input_name_stripped, output_name_stripped) ||
            namesMatchInOutPattern(input_name, output_name) ||
            namesMatchPattern(input_name_stripped, output_name_stripped) ||
            namesMatchPattern(input_name, output_name)) {
        return true;
    }

    return false;
}

// Hotplug events of the two ports of one device arrive separately, in an
// unspecified order, usually within a few milliseconds of each other.
constexpr int kHotplugSettleMs = 300;

} // namespace

// either add input to existing controller or create a new controller
Controller* LibremidiEnumerator::addInput(const libremidi::input_port& inputPort) {
    if (!recognizeDevice(inputPort, m_pConfig)) {
        return nullptr;
    }
    qDebug() << "LibremidiEnumerator::addInput Input added: " << inputPort.port_name.c_str();

    QString inputName = inputPort.port_name.c_str();

    for (auto& pDevice : m_devices) {
        auto* port = pDevice->inputPort();
        if (port && port->port_name == inputPort.port_name) {
            // Already known, e.g. announced by the observer as well as by the
            // initial enumeration.
            return nullptr;
        }
    }

    for (auto& pDevice : m_devices) {
        auto* port = pDevice->inputPort();
        auto* pOutput = pDevice->outputPort();
        if (port || !pOutput) {
            continue;
        }

        QString outputName = pOutput->port_name.c_str();
        if (shouldLinkInputToOutput(inputName, outputName)) {
            pDevice->setInputPort(m_observer, inputPort);
            // The output appeared first (hotplug order is not guaranteed), so
            // the device was added without an input: set it up once the other
            // port events have settled.
            if (!m_enumerating) {
                scheduleSetUp(pDevice.get(), false);
            }
            // input was not a distinct device, we should update the existing device
            return nullptr;
        }
    }

    m_devices.push_back(std::make_unique<LibremidiController>(inputPort.port_name));
    auto& pController = m_devices.back();
    pController->setInputPort(m_observer, inputPort);
    return pController.get();
}

void LibremidiEnumerator::removeInput(const libremidi::input_port& inputPort) {
    qDebug() << "LibremidiEnumerator::removeInput Input removed: " << inputPort.port_name.c_str();
    for (auto it = m_devices.begin(); it != m_devices.end(); it++) {
        LibremidiController* pDevice = it->get();
        auto* pInputPort = pDevice->inputPort();

        if (!pInputPort || pInputPort->port_name != inputPort.port_name) {
            continue;
        }

        auto* pOutputPort = pDevice->outputPort();
        if (pOutputPort) {
            // qDebug() << inputPort.port_name << " " << inputPort.port << "
            // removed from " << device->getName() << " " <<
            // device->m_pInputPort.value().port;
            pDevice->removeInputPort();
            break;
        } else {
            // qDebug() << inputPort.port_name << " " << inputPort.port << "
            // removed with " << device->getName() << " " <<
            // device->m_pInputPort.value().port;
            removeDevice(it);
            break;
        }
    }
}

// either add output to existing controller or create a new controller
Controller* LibremidiEnumerator::addOutput(const libremidi::output_port& outputPort) {
    if (!recognizeDevice(outputPort, m_pConfig)) {
        return nullptr;
    }
    qDebug() << "LibremidiEnumerator::addOutput Output added: " << outputPort.port_name.c_str();

    QString outputName = outputPort.port_name.c_str();
    for (auto& pDevice : m_devices) {
        auto* pKnown = pDevice->outputPort();
        if (pKnown && pKnown->port_name == outputPort.port_name) {
            return nullptr;
        }
    }

    for (auto& pDevice : m_devices) {
        auto* pKnown = pDevice->outputPort();
        auto* pInput = pDevice->inputPort();
        if (pKnown || !pInput) {
            continue;
        }

        QString portName = pInput->port_name.c_str();
        if (shouldLinkInputToOutput(portName, outputPort.port_name.c_str())) {
            // qDebug() << inputName << " matched with " << outputName;
            pDevice->setOutputPort(m_observer, outputPort);
            // During the initial enumeration ControllerManager::slotSetUpDevices()
            // sets up every device afterwards; don't open it twice. On hotplug
            // the input usually arrived first and the device is scheduled
            // already; it then opens once, with both ports.
            if (!m_enumerating) {
                scheduleSetUp(pDevice.get(), false);
            }
            // output was not a distinct device, we should update the existing device
            return nullptr;
        }
    }

    m_devices.push_back(std::make_unique<LibremidiController>(outputPort.port_name));
    auto& pController = m_devices.back();
    pController->setOutputPort(m_observer, outputPort);
    return pController.get();
}

void LibremidiEnumerator::removeOutput(const libremidi::output_port& outputPort) {
    qDebug() << "LibremidiEnumerator::removeOutput Output removed: "
             << outputPort.port_name.c_str();
    for (auto it = m_devices.begin(); it != m_devices.end(); it++) {
        LibremidiController* pDevice = it->get();

        libremidi::output_port* pOutputPort = pDevice->outputPort();
        if (!pOutputPort || pOutputPort->port_name != outputPort.port_name) {
            continue;
        }

        libremidi::input_port* pInputPort = pDevice->inputPort();
        if (pInputPort) {
            // qDebug() << outputPort.port_name << " removed from " << device->getName();
            pDevice->removeOutputPort();
            break;
        } else {
            // contains only output port, remove device
            // qDebug() << outputPort.port_name << " removed with " << device->getName();
            removeDevice(it);
            break;
        }
    }
}

void LibremidiEnumerator::removeDevice(
        std::vector<std::unique_ptr<LibremidiController>>::iterator it) {
    LibremidiController* pDevice = it->get();
    // A hotplugged device that was never handed to the ControllerManager is
    // simply dropped; otherwise close it first and let the manager forget it.
    if (!forgetScheduled(pDevice)) {
        m_pControllerManager->removeDevice(pDevice);
    }
    it->release()->deleteLater();
    m_devices.erase(it);
}

void LibremidiEnumerator::scheduleSetUp(LibremidiController* pDevice, bool isNew) {
    auto contains = [pDevice](const std::vector<LibremidiController*>& v) {
        return std::find(v.begin(), v.end(), pDevice) != v.end();
    };
    // A device still waiting to be added is set up by addDevice() anyway.
    if (isNew) {
        if (!contains(m_scheduledNew)) {
            m_scheduledNew.push_back(pDevice);
        }
    } else if (!contains(m_scheduledNew) && !contains(m_scheduledSetUp)) {
        m_scheduledSetUp.push_back(pDevice);
    }
    // (Re)start: wait until no further port event arrives.
    m_settleTimer.start();
}

bool LibremidiEnumerator::forgetScheduled(LibremidiController* pDevice) {
    std::erase(m_scheduledSetUp, pDevice);
    return std::erase(m_scheduledNew, pDevice) > 0;
}

void LibremidiEnumerator::flushScheduled() {
    auto added = std::move(m_scheduledNew);
    auto setUp = std::move(m_scheduledSetUp);
    m_scheduledNew.clear();
    m_scheduledSetUp.clear();
    for (LibremidiController* pDevice : added) {
        m_pControllerManager->addDevice(pDevice);
    }
    for (LibremidiController* pDevice : setUp) {
        m_pControllerManager->setUpDevice(*pDevice);
    }
}

LibremidiEnumerator::LibremidiEnumerator(
        UserSettingsPointer pConfig, ControllerManager* pControllerManager)
        : m_pConfig(pConfig),
          m_pControllerManager(pControllerManager) {
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(kHotplugSettleMs);
    connect(&m_settleTimer, &QTimer::timeout, this, &LibremidiEnumerator::flushScheduled);

    m_observer = libremidi::observer{
            libremidi::observer_configuration{
                    .input_added =
                            [this](const libremidi::input_port& port) {
                                QMetaObject::invokeMethod(this, [this, port]() {
                                    if (!m_queried) {
                                        return; // covered by queryDevices()
                                    }
                                    if (auto* pController = addInput(port)) {
                                        scheduleSetUp(static_cast<LibremidiController*>(
                                                              pController),
                                                true);
                                    }
                                });
                            },
                    .input_removed =
                            [this](const libremidi::input_port& port) {
                                QMetaObject::invokeMethod(this,
                                        &LibremidiEnumerator::removeInput,
                                        port);
                            },
                    .output_added =
                            [this](const libremidi::output_port& port) {
                                QMetaObject::invokeMethod(this, [this, port]() {
                                    if (!m_queried) {
                                        return; // covered by queryDevices()
                                    }
                                    if (auto* pController = addOutput(port)) {
                                        scheduleSetUp(static_cast<LibremidiController*>(
                                                              pController),
                                                true);
                                    }
                                });
                            },
                    .output_removed =
                            [this](const libremidi::output_port& port) {
                                QMetaObject::invokeMethod(this,
                                        &LibremidiEnumerator::removeOutput,
                                        port);
                            },
                    .track_any = true,
                    .notify_in_constructor = false,
            },
    };

    qDebug() << "Using libremidi backend:"
             << libremidi::get_api_display_name(m_observer.get_current_api()).data();
}

QList<Controller*> LibremidiEnumerator::queryDevices() {
    QList<Controller*> controllers;
    m_queried = true;
    if (m_devices.empty()) {
        m_enumerating = true;
        const auto inputPorts = m_observer.get_input_ports();
        const auto outputPorts = m_observer.get_output_ports();

        for (const auto& inputPort : inputPorts) {
            addInput(inputPort);
        }

        for (const auto& outputPort : outputPorts) {
            addOutput(outputPort);
        }
        m_enumerating = false;
    }

    for (const auto& controller : m_devices) {
        controllers.append(controller.get());
    }

    return controllers;
}

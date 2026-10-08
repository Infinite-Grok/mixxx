#include "controllers/midi/libremidicontroller.h"

#include <QMetaObject>
#include <libremidi/libremidi.hpp>

#include "controllers/midi/libremidimessage.h"
#include "controllers/midi/midiutils.h"
#include "moc_libremidicontroller.cpp"
#include "util/duration.h"

LibremidiController::LibremidiController(std::string name)
        : MidiController(QString::fromStdString(name)) {
}

LibremidiController::~LibremidiController() {
    if (isOpen()) {
        close();
    }
}

void LibremidiController::removeInputPort() {
    m_pInputDevice.reset();
    // Forget the port too: the enumerator uses inputPort() to decide whether
    // the device still has any port left.
    m_pInputPort.reset();
    setInputDevice(false);
}

void LibremidiController::setInputPort(libremidi::observer& observer, libremidi::input_port port) {
    m_pInputPort = port;

    setInputDevice(true);
    m_pInputDevice = libremidi::midi_in{
            libremidi::input_configuration{
                    .on_message = [this](const libremidi::message& m) {
                        this->onMessage(m);
                    },
                    // libremidi drops SysEx, MIDI clock and active sensing by
                    // default. Controllers (e.g. the Hercules T7) answer with
                    // SysEx and mappings may rely on any of these, as they do
                    // with PortMidi, so deliver everything.
                    .ignore_sysex = false,
                    .ignore_timing = false,
                    .ignore_sensing = false,
            },
            libremidi::midi_in_configuration_for(observer)};
}

void LibremidiController::removeOutputPort() {
    m_pOutputDevice.reset();
    m_pOutputPort.reset();
    setOutputDevice(false);
}

void LibremidiController::setOutputPort(
        libremidi::observer& observer, libremidi::output_port port) {
    m_pOutputPort = std::move(port);
    m_pOutputDevice.emplace(libremidi::output_configuration{},
            libremidi::midi_out_configuration_for(observer));
    setOutputDevice(true);
}

void LibremidiController::onMessage(const libremidi::message& m) {
    LibremidiShortMessage msg;
    if (!libremidiUnpackShortMessage(
                std::span<const unsigned char>(m.bytes.data(), m.bytes.size()), &msg)) {
        return;
    }
    const unsigned char status = msg.status;
    auto timestamp = mixxx::Duration::fromNanos(m.timestamp);

    if ((status & 0xF8) == 0xF8) {
        // Handle real-time MIDI messages at any time
        QMetaObject::invokeMethod(this, [=, this] {
            receivedShortMessage(status, 0, 0, timestamp);
        });
        return;
    }

    bool is_sysex = status == 0xF0;
    if (is_sysex) {
        // onMessage() runs on libremidi's input thread: receive() drives the
        // script engine, which belongs to the controller thread. Hand over a
        // deep copy, the message buffer does not outlive this callback.
        const auto* pData = reinterpret_cast<const char*>(m.bytes.data());
        QByteArray data(pData, static_cast<qsizetype>(m.bytes.size()));
        QMetaObject::invokeMethod(this, [this, data = std::move(data), timestamp] {
            receive(data, timestamp);
        });
    } else {
        // unsigned char channel = status & 0x0F;
        const unsigned char note = msg.byte1;
        const unsigned char velocity = msg.byte2;
        QMetaObject::invokeMethod(this, [=, this] {
            receivedShortMessage(status, note, velocity, timestamp);
        });
    }
}

int LibremidiController::open(const QString& resourcePath) {
    if (isOpen()) {
        qCWarning(m_logBase) << "Libremidi device" << getName() << "already open";
        return -1;
    }

    if (getName() == MIXXX_LIBREMIDI_NO_DEVICE_STRING) {
        return -1;
    }

    if (m_pInputPort && m_pInputDevice && isInputDevice()) {
        qCInfo(m_logBase) << "LibremidiController: Opening"
                          << m_pInputPort->port_name.c_str() << "for input";
        stdx::error error = m_pInputDevice->open_port(m_pInputPort.value());

        if (error.is_set()) {
            qCWarning(m_logBase) << "Libremidi error:" << error.message().data();
            return -2;
        }
    }
    if (m_pOutputPort && m_pOutputDevice && isOutputDevice()) {
        qCInfo(m_logBase) << "LibremidiController: Opening"
                          << m_pOutputPort->port_name.c_str() << "for output";

        stdx::error error = m_pOutputDevice->open_port(m_pOutputPort.value());
        if (error.is_set()) {
            qCWarning(m_logBase) << "Libremidi error:" << error.message().data();
            return -2;
        }
    }
    startEngine();
    applyMapping(resourcePath);
    setOpen(true);
    return 0;
}

int LibremidiController::close() {
    if (!isOpen()) {
        qCWarning(m_logBase) << "Libremidi device" << getName() << "already closed";
        return -1;
    }

    stopEngine();
    MidiController::close();

    int result = 0;

    if (m_pInputDevice && m_pInputDevice->is_port_connected()) {
        stdx::error error = m_pInputDevice->close_port();
        if (error.is_set()) {
            qCWarning(m_logBase) << "Libremidi error:" << error.message().data();
            result = -1;
        }
    }

    if (m_pOutputDevice && m_pOutputDevice->is_port_open()) {
        stdx::error error = m_pOutputDevice->close_port();
        if (error.is_set()) {
            qCWarning(m_logBase) << "Libremidi error:" << error.message().data();
            result = -1;
        }
    }

    setOpen(false);
    return result;
}

void LibremidiController::sendShortMsg(
        unsigned char status, unsigned char byte1, unsigned char byte2) {
    if (!m_pOutputPort || !m_pOutputDevice || !m_pOutputDevice->is_port_open()) {
        return;
    }

    stdx::error error = m_pOutputDevice->send_message(status, byte1, byte2);
    if (error.is_set()) {
        // Use two qWarnings() to ensure line break works on all operating systems
        qCWarning(m_logOutput) << "Error sending short message"
                               << MidiUtils::formatMidiOpCode(getName(),
                                          status,
                                          byte1,
                                          byte2,
                                          MidiUtils::channelFromStatus(status),
                                          MidiUtils::opCodeFromStatus(status));
        qCWarning(m_logOutput) << "Libremidi error:" << error.message().data();
    } else {
        qDebug(m_logOutput) << QStringLiteral("outgoing: ")
                            << MidiUtils::formatMidiOpCode(getName(),
                                       status,
                                       byte1,
                                       byte2,
                                       MidiUtils::channelFromStatus(status),
                                       MidiUtils::opCodeFromStatus(status));
    }
}

bool LibremidiController::sendBytes(const QByteArray& data) {
    if (!m_pOutputPort || !m_pOutputDevice || !m_pOutputDevice->is_port_open()) {
        return false;
    }

    stdx::error error = m_pOutputDevice->send_message(
            (unsigned char*)data.constData(), data.size());
    if (error.is_set()) {
        // Use two qWarnings() to ensure line break works on all operating systems
        qCWarning(m_logOutput) << "Error sending SysEx message:"
                               << MidiUtils::formatSysexMessage(getName(), data);
        qCWarning(m_logOutput) << "Libremidi error:" << error.message().data();
    } else {
        qCDebug(m_logOutput) << QStringLiteral("outgoing: ")
                             << MidiUtils::formatSysexMessage(getName(), data);
        return true;
    }
    return false;
}

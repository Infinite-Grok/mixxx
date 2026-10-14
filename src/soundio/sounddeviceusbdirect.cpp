#include "soundio/sounddeviceusbdirect.h"

#include <QtCore/private/qandroidextras_p.h>
#include <android/log.h>
#include <jni.h>

#include <QCoreApplication>
#include <QTimer>
#include <QJniArray>
#include <QThread>
#include <QtDebug>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "control/controlobject.h"
#include "controllers/android.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerutil.h"
#include "util/defs.h"
#include "waveform/visualplayposition.h"

struct SoundDeviceUsbDirect::Shared {
    SoundDeviceUsbDirect* pDevice;
};

namespace {

const QString kAppGroup = QStringLiteral("[App]");
constexpr int kCpuUsageUpdateRate = 30; // 1/s, same as the PortAudio device
// open() never blocks long for the permission dialog: it asks when the device is enumerated.
constexpr int kPermissionWaitMs = 3000;
constexpr int kPermissionReRequestMs = 10000;
constexpr int kHaltPollMs = 200;
// Extra frames the FIFO keeps beyond one engine block (covers render time and wake-up latency).
constexpr size_t kFifoMarginFrames = 256;

QJniObject usbManager() {
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    QJniObject USB_SERVICE = QJniObject::getStaticObjectField(
            "android/content/Context", "USB_SERVICE", "Ljava/lang/String;");
    return context.callObjectMethod("getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;",
            USB_SERVICE.object());
}

// Returns the attached UsbDevice with this VID/PID (invalid QJniObject if none).
QJniObject findUsbDevice(const QJniObject& manager, uint16_t vid, uint16_t pid) {
    QJniObject map = manager.callMethod<QJniObject>("getDeviceList", "()Ljava/util/HashMap;");
    if (!map.isValid()) {
        return QJniObject();
    }
    QJniObject values = map.callMethod<jobject>("values", "()Ljava/util/Collection;");
    QJniArray<QJniObject> list(values.callMethod<jobjectArray>("toArray"));
    for (const auto& pDev : list) {
        if (static_cast<uint16_t>(pDev->callMethod<jint>("getVendorId")) == vid &&
                static_cast<uint16_t>(pDev->callMethod<jint>("getProductId")) == pid) {
            return QJniObject(pDev->object());
        }
    }
    return QJniObject();
}

bool hasUsbPermission(const QJniObject& manager, const QJniObject& device) {
    return manager.callMethod<jboolean>(
            "hasPermission", "(Landroid/hardware/usb/UsbDevice;)Z", device.object());
}

// Non-blocking. Rate-limited so that repeated enumeration or Apply cannot stack dialogs.
void requestUsbPermission(const QJniObject& manager, const QJniObject& device) {
    static std::chrono::steady_clock::time_point lastRequest;
    static bool requested = false;
    const auto now = std::chrono::steady_clock::now();
    if (requested &&
            now - lastRequest < std::chrono::milliseconds(kPermissionReRequestMs)) {
        return;
    }
    requested = true;
    lastRequest = now;
    manager.callMethod<void>("requestPermission",
            "(Landroid/hardware/usb/UsbDevice;Landroid/app/PendingIntent;)V",
            device.object(),
            mixxx::android::getIntent().object());
}

void showNotice(const QString& message) {
    qWarning() << "USB Direct:" << message;
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([message]() {
        QJniObject context = QNativeInterface::QAndroidApplication::context();
        QJniObject text = QJniObject::fromString(message);
        QJniObject toast = QJniObject::callStaticObjectMethod("android/widget/Toast",
                "makeText",
                "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;",
                context.object(),
                text.object<jstring>(),
                1 /* LENGTH_LONG */);
        if (toast.isValid()) {
            toast.callMethod<void>("show");
        }
    });
}

} // namespace

SoundDeviceUsbDirect::SoundDeviceUsbDirect(UserSettingsPointer config,
        SoundManager* sm,
        const usbdirect::DeviceProfile* pProfile,
        unsigned int index)
        : SoundDevice(config, sm),
          m_pProfile(pProfile),
          m_shared(std::make_shared<Shared>(Shared{this})),
          m_isClkRef(false),
          m_frameSize(pProfile->format.channels),
          m_lastEngineUnderruns(0),
          m_lastRenderOverruns(0),
          m_audioLatencyUsage(kAppGroup, QStringLiteral("audio_latency_usage")),
          m_framesSinceUsageUpdate(0) {
    m_hostAPI = SoundManagerConfig::kAPIUsbDirect;
    m_strDisplayName = QString::fromStdString(pProfile->format.displayName) +
            QStringLiteral(" (USB Direct)");
    m_deviceId.name = m_strDisplayName;
    m_deviceId.deviceIndex = kDeviceIndexBase + index;
    m_numOutputChannels = mixxx::audio::ChannelCount(static_cast<int>(pProfile->format.channels));
    m_numInputChannels = mixxx::audio::ChannelCount(0);
    m_sampleRate = getDefaultSampleRate();
}

SoundDeviceUsbDirect::~SoundDeviceUsbDirect() {
    // Queued notifications must not touch a destroyed device (both run on the main thread).
    m_shared->pDevice = nullptr;
    if (isOpen()) {
        close();
    }
}

mixxx::audio::SampleRate SoundDeviceUsbDirect::getDefaultSampleRate() const {
    return mixxx::audio::SampleRate(m_pProfile->format.sampleRate);
}

int SoundDeviceUsbDirect::mainBase() const {
    const auto& f = m_pProfile->format;
    for (unsigned i = 0; i + 1 < f.channels; i++) {
        if (f.channelRoles[i] == usbdirect::ChannelRole::MainL &&
                f.channelRoles[i + 1] == usbdirect::ChannelRole::MainR) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int SoundDeviceUsbDirect::phonesBase() const {
    const auto& f = m_pProfile->format;
    for (unsigned i = 0; i + 1 < f.channels; i++) {
        if (f.channelRoles[i] == usbdirect::ChannelRole::PhonesL &&
                f.channelRoles[i + 1] == usbdirect::ChannelRole::PhonesR) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// static
std::vector<const usbdirect::DeviceProfile*> SoundDeviceUsbDirect::attachedProfiles() {
    std::vector<const usbdirect::DeviceProfile*> found;
    QJniObject manager = usbManager();
    if (!manager.isValid()) {
        return found;
    }
    size_t count = 0;
    const usbdirect::DeviceProfile* table = usbdirect::profiles(&count);
    for (size_t i = 0; i < count; i++) {
        const QJniObject device =
                findUsbDevice(manager, table[i].vendorId, table[i].productId);
        if (device.isValid()) {
            // Ask as soon as the device shows up so the dialog is answered before Apply.
            if (!hasUsbPermission(manager, device)) {
                requestUsbPermission(manager, device);
            }
            found.push_back(&table[i]);
        }
    }
    return found;
}

bool SoundDeviceUsbDirect::acquireConnection(std::vector<unsigned char>* pRaw, int* pFd) {
    QJniObject manager = usbManager();
    if (!manager.isValid()) {
        m_lastError = QStringLiteral("Android UsbManager is not available");
        return false;
    }
    QJniObject device = findUsbDevice(manager, m_pProfile->vendorId, m_pProfile->productId);
    if (!device.isValid()) {
        m_lastError = QStringLiteral("USB device is not attached");
        return false;
    }
    if (!hasUsbPermission(manager, device)) {
        // No nested event loop: the grant is visible through UsbManager without Qt events.
        requestUsbPermission(manager, device);
        const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kPermissionWaitMs);
        while (!hasUsbPermission(manager, device)) {
            if (std::chrono::steady_clock::now() > deadline) {
                m_lastError = QStringLiteral(
                        "Allow USB access for the controller, then press Apply again.");
                return false;
            }
            QThread::msleep(100);
        }
    }
    m_connection = manager.callMethod<jobject>("openDevice",
            "(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;",
            device.object());
    if (!m_connection.isValid()) {
        m_lastError = QStringLiteral("UsbManager.openDevice failed");
        return false;
    }
    QJniArray<jbyte> raw = m_connection.callMethod<QJniArray<jbyte>>("getRawDescriptors");
    pRaw->clear();
    if (raw.isValid()) {
        for (jbyte b : raw) {
            pRaw->push_back(static_cast<unsigned char>(b));
        }
    }
    if (pRaw->empty()) {
        m_lastError = QStringLiteral("USB descriptors are not readable");
        return false;
    }
    *pFd = static_cast<int>(m_connection.callMethod<jint>("getFileDescriptor"));
    return true;
}

SoundDeviceStatus SoundDeviceUsbDirect::open(bool isClkRefDevice, int) {
    qDebug() << "SoundDeviceUsbDirect::open()" << m_deviceId;
    if (isOpen()) {
        return SoundDeviceStatus::Ok;
    }
    m_lastError.clear();
    if (m_audioOutputs.empty()) {
        m_lastError = QStringLiteral("No outputs configured for the USB Direct device");
        return SoundDeviceStatus::Error;
    }
    if (m_sampleRate.value() != m_pProfile->format.sampleRate) {
        m_lastError = QStringLiteral("USB Direct only supports %1 Hz for this device")
                              .arg(m_pProfile->format.sampleRate);
        return SoundDeviceStatus::Error;
    }
    const unsigned block = static_cast<unsigned>(m_configFramesPerBuffer);
    if (block < 16 || block > kMaxEngineFrames) {
        m_lastError = QStringLiteral("Unsupported buffer size %1").arg(block);
        return SoundDeviceStatus::Error;
    }

    m_generation.fetch_add(1);
    m_halted = false;
    m_haltNotified = false;
    m_renderInit = false;
    m_lastEngineUnderruns = 0;
    m_lastRenderOverruns = 0;
    m_framesSinceUsageUpdate = 0;
    m_timeInCallback = mixxx::Duration();
    m_isClkRef = isClkRefDevice;
    m_openerThread = std::this_thread::get_id();

    usbdirect::Options options; // defaults: depth 12 adaptive 4..16, nice -19 event thread
    m_pOut = std::make_unique<usbdirect::UsbAudioOut>(options);

    std::vector<unsigned char> raw;
    int fd = -1;
    if (!acquireConnection(&raw, &fd)) {
        teardown();
        return SoundDeviceStatus::Error;
    }
    const usbdirect::OpenResult rc = m_pOut->open(fd, raw.data(), raw.size());
    if (rc != usbdirect::OpenResult::Ok) {
        m_lastError = QStringLiteral("USB open failed: %1")
                              .arg(QString::fromUtf8(usbdirect::openResultName(rc)));
        teardown();
        return SoundDeviceStatus::Error;
    }

    size_t target = block + kFifoMarginFrames;
    const int cfgTarget = m_pConfig->getValue(
            ConfigKey(QStringLiteral("[Android]"), QStringLiteral("usb_direct_fifo_target_frames")),
            0);
    if (cfgTarget > 0) {
        target = static_cast<size_t>(cfgTarget);
    }
    target = std::min<size_t>(target, 8192);

    m_pPump = std::make_unique<usbdirect::EnginePump>(
            *m_pOut,
            [this](float* pInterleaved, int frames) { render(pInterleaved, frames); },
            block,
            target);
    // Start order: pump (wires low-water, silent prefill on this thread) then transport.
    if (!m_pPump->start()) {
        m_lastError = QStringLiteral("Engine pump could not start");
        teardown();
        return SoundDeviceStatus::Error;
    }
    m_open = true;
    if (!m_pOut->start()) {
        m_open = false;
        m_lastError = QStringLiteral("USB stream could not start");
        teardown();
        return SoundDeviceStatus::Error;
    }

    {
        // Detects a halt even when the engine is no longer rendering (FIFO full, nothing pops).
        const unsigned generation = m_generation.load();
        std::shared_ptr<Shared> shared = m_shared;
        m_pHaltTimer = new QTimer(m_pSoundManager);
        m_pHaltTimer->setInterval(kHaltPollMs);
        QObject::connect(m_pHaltTimer, &QTimer::timeout, m_pSoundManager, [shared, generation]() {
            SoundDeviceUsbDirect* pDevice = shared->pDevice;
            if (pDevice && pDevice->m_open.load() && pDevice->m_generation.load() == generation &&
                    pDevice->m_pOut) {
                const usbdirect::Halt reason = pDevice->m_pOut->stats().reason;
                if (reason != usbdirect::Halt::None && reason != usbdirect::Halt::UserStop) {
                    pDevice->onHalted(generation);
                }
            }
        });
        m_pHaltTimer->start();
    }

    if (isClkRefDevice) {
        const double rate = m_sampleRate.toDouble();
        const double latencyMs = (static_cast<double>(target) + options.startDepth * rate / 1000.0) /
                rate * 1000.0;
        ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("output_latency_ms")), latencyMs);
        ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("samplerate")), m_sampleRate);
        m_clkRefTimer.start();
    }
    qInfo() << "USB Direct opened:" << m_strDisplayName << "block" << block << "fifo target"
            << target << "clock reference" << isClkRefDevice;
    return SoundDeviceStatus::Ok;
}

bool SoundDeviceUsbDirect::isOpen() const {
    return m_open.load();
}

void SoundDeviceUsbDirect::teardown() {
    m_open = false;
    if (m_pHaltTimer) {
        m_pHaltTimer->stop();
        m_pHaltTimer->deleteLater(); // may be running inside its own timeout
        m_pHaltTimer = nullptr;
    }
    bool replugNeeded = false;
    // 1. Transport: stop() blocks until every transfer is reaped and the interface is released.
    if (m_pOut) {
        m_pOut->stop();
        if (m_pOut->detachedKernelDriver()) {
            for (int i = 0; i < m_pOut->ownershipCount(); i++) {
                // tag 9 = libusb_attach_kernel_driver result
                if (m_pOut->ownershipTag(i) == 9 && m_pOut->ownershipReturn(i) != 0) {
                    replugNeeded = true;
                }
            }
        }
    }
    // 2. Engine pump: only after the transport can no longer call the low-water thunk.
    if (m_pPump) {
        m_pPump->stop();
        qInfo() << "USB Direct closed: blocks" << m_pPump->blocksRendered() << "render overruns"
                << m_pPump->renderOverruns();
        m_pPump.reset();
    }
    if (m_pOut) {
        const usbdirect::Stats s = m_pOut->stats();
        qInfo() << "USB Direct stats: depth" << s.depth << "depthChanges" << s.depthChanges
                << "engineUnderruns" << static_cast<qulonglong>(s.engineUnderruns) << "errors"
                << static_cast<qulonglong>(s.errors) << "gapExceedDepth"
                << static_cast<qulonglong>(s.gapExceedDepth) << "halt"
                << usbdirect::haltName(s.reason);
        m_pOut.reset();
    }
    // 3. Only now the Java connection (its fd) may be closed.
    if (m_connection.isValid()) {
        m_connection.callMethod<void>("close");
        m_connection = QJniObject();
    }
    if (replugNeeded && !m_halted.load()) {
        showNotice(QStringLiteral("Replug the controller to restore normal Android audio."));
    }
}

SoundDeviceStatus SoundDeviceUsbDirect::close() {
    qDebug() << "SoundDeviceUsbDirect::close()" << m_deviceId;
    teardown();
    return SoundDeviceStatus::Ok;
}

void SoundDeviceUsbDirect::onHalted(unsigned generation) {
    if (!m_open.load() || generation != m_generation.load()) {
        return;
    }
    const usbdirect::Halt reason = m_pOut ? m_pOut->stats().reason : usbdirect::Halt::None;
    m_lastError = QStringLiteral("USB audio stream stopped (%1)")
                          .arg(QString::fromUtf8(usbdirect::haltName(reason)));
    showNotice(m_lastError +
            QStringLiteral(". Reselect the sound output in Preferences or replug the controller."));
    // Stops and reaps this device (and any other open ones), marks it as the error device and
    // reports the sound status as disconnected.
    m_pSoundManager->deviceFailed(this);
}

void SoundDeviceUsbDirect::render(float* pInterleaved, int frames) {
    const SINT n = frames;
    // The silent prefill runs on the thread that opened the device (before the engine thread
    // exists); the Mixxx engine must only ever be driven from the pump thread.
    if (std::this_thread::get_id() == m_openerThread || m_halted.load(std::memory_order_relaxed)) {
        std::fill(pInterleaved, pInterleaved + static_cast<size_t>(n) * m_frameSize, 0.0f);
        return;
    }
    m_clkRefTimer.restart();

    if (!m_renderInit.exchange(true)) {
#if defined(__aarch64__)
        int64_t savedFPCR;
        asm volatile("mrs %[savedFPCR], FPCR" : [savedFPCR] "=r"(savedFPCR));
        asm volatile("msr FPCR, %[src]" : : [src] "r"(savedFPCR | (1 << 24)));
#endif
    }

    const usbdirect::Stats s = m_pOut->stats();
    if (s.reason != usbdirect::Halt::None) {
        m_halted.store(true);
        std::fill(pInterleaved, pInterleaved + static_cast<size_t>(n) * m_frameSize, 0.0f);
        if (s.reason != usbdirect::Halt::UserStop && m_open.load() &&
                !m_haltNotified.exchange(true)) {
            // Once per halt, from the engine thread; never stops anything from here.
            std::shared_ptr<Shared> shared = m_shared;
            const unsigned generation = m_generation.load();
            QMetaObject::invokeMethod(
                    m_pSoundManager,
                    [shared, generation]() {
                        if (shared->pDevice) {
                            shared->pDevice->onHalted(generation);
                        }
                    },
                    Qt::QueuedConnection);
        }
        return;
    }

    if (m_isClkRef) {
        const double secs = static_cast<double>(s.usbQueuedFrames + m_pOut->fifo().size()) /
                m_sampleRate.toDouble();
        VisualPlayPosition::setCallbackEntryToDacSecs(secs, m_clkRefTimer);
    }
    if (s.engineUnderruns != m_lastEngineUnderruns) {
        m_lastEngineUnderruns = s.engineUnderruns;
        m_pSoundManager->underflowHappened(8);
    }
    const uint64_t overruns = m_pPump ? m_pPump->renderOverruns() : 0;
    if (overruns != m_lastRenderOverruns) {
        m_lastRenderOverruns = overruns;
        m_pSoundManager->underflowHappened(9);
    }
    m_pSoundManager->processUnderflowHappened(n);
    m_pSoundManager->readProcess(n);
    m_pSoundManager->onDeviceOutputCallback(n);
    composeOutputBuffer(pInterleaved, n, 0, static_cast<int>(m_frameSize));
    m_pSoundManager->writeProcess(n);

    m_framesSinceUsageUpdate += n;
    if (m_framesSinceUsageUpdate > m_sampleRate.toDouble() / kCpuUsageUpdateRate) {
        const double secInCb = m_timeInCallback.toDoubleSeconds();
        m_audioLatencyUsage.set(secInCb / (m_framesSinceUsageUpdate / m_sampleRate.toDouble()));
        m_timeInCallback = mixxx::Duration::fromSeconds(0);
        m_framesSinceUsageUpdate = 0;
    }
    m_timeInCallback += m_clkRefTimer.elapsed();
}

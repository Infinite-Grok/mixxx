#include "waveform/visualplayposition.h"

#include <atomic>
#include <chrono>

#include "moc_visualplayposition.cpp"
#include "util/cmdlineargs.h"
#include "util/math.h"
#include "waveform/isynctimeprovider.h"

//static
QMap<QString, QWeakPointer<VisualPlayPosition>> VisualPlayPosition::m_listVisualPlayPosition;
PerformanceTimer VisualPlayPosition::m_timeInfoTime;
double VisualPlayPosition::m_dCallbackEntryToDacSecs = 0;

namespace {

// At most one warning line per 10 s for all decks and devices; the next line reports how many were suppressed.
class RateLimitedLog {
  public:
    bool allow(unsigned* pSuppressed) {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
        int64_t next = m_nextMs.load(std::memory_order_relaxed);
        if (nowMs >= next &&
                m_nextMs.compare_exchange_strong(next, nowMs + kIntervalMs)) {
            *pSuppressed = m_suppressed.exchange(0);
            return true;
        }
        m_suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

  private:
    static constexpr int64_t kIntervalMs = 10000;
    std::atomic<int64_t> m_nextMs{0};
    std::atomic<unsigned> m_suppressed{0};
};

RateLimitedLog s_outdatedLog;
RateLimitedLog s_noTransportLog;

} // namespace

VisualPlayPosition::VisualPlayPosition(const QString& key)
        : m_key{key},
          m_noTransport{false} {
}

VisualPlayPosition::~VisualPlayPosition() {
    if (!m_key.isEmpty()) {
        m_listVisualPlayPosition.remove(m_key);
    }
}

void VisualPlayPosition::set(
        double playPosition,
        double playRate,
        double positionStep,
        double slipPosition,
        double slipRate,
        SlipModeState m_slipModeState,
        bool loopEnabled,
        bool loopInAdjustActive,
        bool loopOutAdjustActive,
        double loopStartPosition,
        double loopEndPosition,
        double tempoTrackSeconds,
        double audioBufferMicroS) {
    VisualPlayPositionData data;
    data.m_referenceTime = m_timeInfoTime;
    data.m_callbackEntrytoDac = static_cast<int>(m_dCallbackEntryToDacSecs * 1000000); // s to µs
    data.m_playPos = playPosition;
    data.m_playRate = playRate;
    data.m_slipRate = slipRate;
    data.m_positionStep = positionStep;
    data.m_slipPos = slipPosition;
    data.m_slipModeState = m_slipModeState;
    data.m_loopEnabled = loopEnabled;
    data.m_loopInAdjustActive = loopInAdjustActive;
    data.m_loopOutAdjustActive = loopOutAdjustActive;
    data.m_loopStartPos = loopStartPosition;
    data.m_loopEndPos = loopEndPosition;
    data.m_tempoTrackSeconds = tempoTrackSeconds;
    data.m_audioBufferMicroS = audioBufferMicroS;

    m_data.push(data);
}

double VisualPlayPosition::calcOffsetAtNextVSync(
        VSyncTimeProvider* pSyncTimeProvider, const VisualPlayPositionData& data) {
    if (data.m_audioBufferMicroS != 0.0) {
        int refToVSync = pSyncTimeProvider->fromTimerToNextSync(data.m_referenceTime).count();
        int syncIntervalTimeMicros = pSyncTimeProvider->getSyncInterval().count();
        // The positive offset is limited to the audio buffer + 2 x waveform sync interval
        // This should be sufficient to compensate jitter, but does not continue
        // in case of underflows.
        const int maxOffsetMicros = static_cast<int>(
                data.m_audioBufferMicroS + 2 * syncIntervalTimeMicros);

        // The minimum offset is limited to -data.m_callbackEntrytoDac to avoid a more
        // negative value indicating an outdated request that is no longer valid anyway.
        // This is probably caused by a vsync problem.
        const int minOffsetMicros = -data.m_callbackEntrytoDac;

        // Calculate the offset in micros for the position of the sample that will be transferred
        // to the DAC when the next display frame is displayed
        int offsetMicros = refToVSync - data.m_callbackEntrytoDac;
        if (offsetMicros < minOffsetMicros) {
            offsetMicros = minOffsetMicros;
            if (!m_noTransport) {
                unsigned suppressed = 0;
                if (s_outdatedLog.allow(&suppressed)) {
                    qWarning() << "VisualPlayPosition::calcOffsetAtNextVSync"
                               << m_key << "outdated position request (offset < minOffset),"
                               << suppressed << "similar warnings suppressed";
                }
                qDebug() << m_key << "refToVSync:" << refToVSync
                         << "data.m_callbackEntrytoDac:"
                         << data.m_callbackEntrytoDac;
                m_noTransport = true;
            }
        } else if (offsetMicros > maxOffsetMicros) {
            offsetMicros = maxOffsetMicros;
            if (!m_noTransport) {
                unsigned suppressed = 0;
                if (s_noTransportLog.allow(&suppressed)) {
                    qWarning() << "VisualPlayPosition::calcOffsetAtNextVSync"
                               << m_key << "no transport (offsetMicros > maxOffsetMicros),"
                               << suppressed << "similar warnings suppressed";
                }
                qDebug() << m_key << "refToVSync:" << refToVSync
                         << "data.m_callbackEntrytoDac:"
                         << data.m_callbackEntrytoDac;
                m_noTransport = true;
            }
        } else {
            if (m_noTransport) {
                qDebug() << m_key << "refToVSync:" << refToVSync
                         << "data.m_callbackEntrytoDac:"
                         << data.m_callbackEntrytoDac;
            }
            m_noTransport = false;
        }
        // Apply the offset proportional to m_positionStep
        return static_cast<double>(offsetMicros) / data.m_audioBufferMicroS;
    }
    return 0.0;
}

double VisualPlayPosition::determinePlayPosInLoopBoundries(
        const VisualPlayPositionData& data, const double& offsetSteps) {
    double interpolatedPlayPos = data.m_playPos + offsetSteps * data.m_playRate;

    if (data.m_loopEnabled) {
        double loopSize = data.m_loopEndPos - data.m_loopStartPos;
        if (loopSize > 0) {
            if ((data.m_playRate < 0.0) &&
                    (interpolatedPlayPos < data.m_loopStartPos) &&
                    (data.m_playPos >= data.m_loopStartPos) &&
                    !data.m_loopInAdjustActive) {
                // 1. Deck playing reverse
                // 2. Interpolated playposition at the time of next VSync would
                // be outsite of the active loop
                // 3. Playposition is currently inside the active loop
                //    (not scratching left of an activated loop)
                // 4. LoopIn is not being held down
                interpolatedPlayPos = data.m_loopEndPos -
                        std::remainder(
                                data.m_loopStartPos - interpolatedPlayPos,
                                loopSize);
            }
            if ((data.m_playRate > 0.0) &&
                    (interpolatedPlayPos > data.m_loopEndPos) &&
                    (data.m_playPos <= data.m_loopEndPos) &&
                    !data.m_loopOutAdjustActive) {
                // 1. Deck playing forward
                // 2. Interpolated playposition at the time of next VSync would
                // be outsite of the active loop
                // 3. Playposition is currently inside the active loop
                //    (not scratching right of an activated loop)
                // 4. LoopOut is not being held down
                interpolatedPlayPos = data.m_loopStartPos +
                        std::remainder(
                                interpolatedPlayPos - data.m_loopEndPos,
                                loopSize);
            }
        }
    }
    return interpolatedPlayPos;
}

bool VisualPlayPosition::getPlaySlipAtNextVSync(
        VSyncTimeProvider* pSyncTimeProvider,
        double* pPlayPosition,
        double* pSlipPosition) {
    VisualPlayPositionData data;
    std::size_t i = 0;
    double offsetBuffers = 0;
    // Work around #15886: Don't use the most recent buffer right away because
    // it likely not reached the DAC. This has caused a visible jump back, from
    // the correct pause position to a too early position when start playing.
    for (; i < 3; ++i) {
        // Find buffer that is currently in the DAC.
        // This is either at 0 or 1, but can also be at 2, if the buffer has
        // been updated conurrently during the loop
        if (m_data.getAt(i, &data)) {
            offsetBuffers = calcOffsetAtNextVSync(pSyncTimeProvider, data);
            if (offsetBuffers > -1) {
                // The buffer, currently in the DAC has an offset of -1 ... 0
                break;
            }
        }
    }
    if (i >= 3) {
        // No valid data available e.g, track ejected
        return false;
    }

    const double offsetSteps = data.m_positionStep * offsetBuffers;
    double interpolatedPlayPos = determinePlayPosInLoopBoundries(data, offsetSteps);
    *pPlayPosition = interpolatedPlayPos;

    if (data.m_slipModeState == SlipModeState::Running) {
        *pSlipPosition = data.m_slipPos + offsetSteps * data.m_slipRate;
    } else {
        *pSlipPosition = interpolatedPlayPos;
    }
    return true;
}

double VisualPlayPosition::getEnginePlayPos() {
    VisualPlayPositionData data;
    if (m_data.getAt(0, &data)) {
        return data.m_playPos;
    } else {
        return 0;
    }
}

void VisualPlayPosition::getTrackTime(double* pPlayPosition, double* pTempoTrackSeconds) {
    VisualPlayPositionData data;
    if (m_data.getAt(0, &data)) {
        *pPlayPosition = data.m_playPos;
        *pTempoTrackSeconds = data.m_tempoTrackSeconds;
    } else {
        *pPlayPosition = 0;
        *pTempoTrackSeconds = 0;
    }
}

//static
QSharedPointer<VisualPlayPosition> VisualPlayPosition::getVisualPlayPosition(const QString& group) {
    QSharedPointer<VisualPlayPosition> vpp = m_listVisualPlayPosition.value(group);
    if (vpp.isNull()) {
        vpp = QSharedPointer<VisualPlayPosition>(new VisualPlayPosition(group));
        m_listVisualPlayPosition.insert(group, vpp);
    }
    return vpp;
}

//static
void VisualPlayPosition::setCallbackEntryToDacSecs(double secs, const PerformanceTimer& time) {
    // the time is valid only just NOW, so measure the time from NOW for
    // later correction
    m_timeInfoTime = time;
    m_dCallbackEntryToDacSecs = secs;
}

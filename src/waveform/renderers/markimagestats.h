#pragma once

// Test-build instrumentation (I6b): counts why waveform mark images are
// flagged for regeneration and how often they are really regenerated.
// One qInfo line per 10 s, only when something was counted.

#include <QHash>
#include <QString>
#include <QStringList>
#include <QtDebug>
#include <chrono>
#include <mutex>

namespace MarkImageStats {

enum Cause { Breadth, Level, Text, Color, Jump, Other, CauseCount };

struct Counts {
    quint64 cause[CauseCount]{};
    quint64 image{};
    quint64 endImage{};
    bool any() const {
        if (image || endImage) {
            return true;
        }
        for (quint64 c : cause) {
            if (c) {
                return true;
            }
        }
        return false;
    }
};

struct State {
    std::mutex mutex;
    QHash<QString, Counts> perGroup;
    std::chrono::steady_clock::time_point lastLog{std::chrono::steady_clock::now()};
};

inline State& state() {
    static State s;
    return s;
}

// Label of the renderer currently running update()/resize/cue update on this
// thread (empty outside those).
inline QString& currentGroup() {
    thread_local QString group;
    return group;
}

class Scope {
  public:
    explicit Scope(const QString& group)
            : m_previous(currentGroup()) {
        currentGroup() = group;
    }
    ~Scope() {
        currentGroup() = m_previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    QString m_previous;
};

inline Counts& countsForCurrent(State& s) {
    const QString& g = currentGroup();
    return s.perGroup[g.isEmpty() ? QStringLiteral("?") : g];
}

inline void noteCause(Cause cause) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    countsForCurrent(s).cause[cause]++;
}

inline void noteImage(bool endImage) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Counts& c = countsForCurrent(s);
    (endImage ? c.endImage : c.image)++;
}

inline void logIfDue() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - s.lastLog < std::chrono::seconds(10)) {
        return;
    }
    s.lastLog = now;
    QStringList parts;
    for (auto it = s.perGroup.begin(); it != s.perGroup.end(); ++it) {
        const Counts& c = it.value();
        if (!c.any()) {
            continue;
        }
        parts << QStringLiteral(
                "%1 breadth=%2 level=%3 text=%4 color=%5 jump=%6 other=%7 "
                "img=%8 endimg=%9")
                         .arg(it.key())
                         .arg(c.cause[Breadth])
                         .arg(c.cause[Level])
                         .arg(c.cause[Text])
                         .arg(c.cause[Color])
                         .arg(c.cause[Jump])
                         .arg(c.cause[Other])
                         .arg(c.image)
                         .arg(c.endImage);
        it.value() = Counts{};
    }
    if (!parts.isEmpty()) {
        qInfo().noquote() << "MarkImageStats 10s:" << parts.join(QStringLiteral(" | "));
    }
}

} // namespace MarkImageStats

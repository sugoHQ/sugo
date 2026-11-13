// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionKeeper.h"

#include "Database.h"
#include "PresetModel.h"
#include "SessionRecorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QTimeZone>

Q_LOGGING_CATEGORY(lcSessionKeeper, "sugo.session.keeper")

namespace
{
constexpr int kFormat = 1;

QString phaseName(TimerEngine::Phase phase)
{
    switch (phase) {
    case TimerEngine::Phase::Work:
        return QStringLiteral("work");
    case TimerEngine::Phase::ShortBreak:
        return QStringLiteral("short_break");
    case TimerEngine::Phase::LongBreak:
        return QStringLiteral("long_break");
    }
    return QStringLiteral("work");
}

bool phaseFromName(const QString &name, TimerEngine::Phase *phase)
{
    for (const auto candidate : {TimerEngine::Phase::Work, TimerEngine::Phase::ShortBreak, TimerEngine::Phase::LongBreak}) {
        if (name == phaseName(candidate)) {
            *phase = candidate;
            return true;
        }
    }
    return false;
}

SessionKind kindFor(TimerEngine::Phase phase)
{
    switch (phase) {
    case TimerEngine::Phase::Work:
        return SessionKind::Work;
    case TimerEngine::Phase::ShortBreak:
        return SessionKind::ShortBreak;
    case TimerEngine::Phase::LongBreak:
        return SessionKind::LongBreak;
    }
    return SessionKind::Work;
}
} // namespace

SessionKeeper::SessionKeeper(QObject *parent)
    : QObject(parent)
    , m_now([]() { return QDateTime::currentDateTimeUtc(); })
{
    m_heartbeat.setInterval(kHeartbeatMs);
    connect(&m_heartbeat, &QTimer::timeout, this, &SessionKeeper::save);
}

QString SessionKeeper::defaultPath()
{
    return QFileInfo(Database::defaultPath()).dir().filePath(QStringLiteral("timer-session.json"));
}

void SessionKeeper::attach(TimerEngine *engine, PresetModel *presets, SessionRecorder *recorder, const QString &path)
{
    m_engine = engine;
    m_presets = presets;
    m_recorder = recorder;
    m_path = path;
    if (!engine) {
        return;
    }
    // The timer as it was when the phase began: editing it meanwhile does not change the session's
    // name or category (as SessionRecorder).
    captureTimer();
    // phaseStarted comes after the state and phase changes that saved the new phase: it is saved
    // again with its own timer, or a crash before the next heartbeat would bring it back under
    // the timer that ran before.
    connect(engine, &TimerEngine::phaseStarted, this, [this]() {
        captureTimer();
        save();
    });
    for (const auto signal : {&TimerEngine::stateChanged, &TimerEngine::phaseChanged, &TimerEngine::totalChanged,
                              &TimerEngine::cyclesChanged}) {
        connect(engine, signal, this, &SessionKeeper::save);
    }
    // What became of a session brought back: ending it (stop, skip) reports a session first;
    // resuming it only changes the state.
    connect(engine, &TimerEngine::sessionEnded, this, [this]() {
        finishTracking(QStringLiteral("stopped"));
    });
    connect(engine, &TimerEngine::stateChanged, this, [this]() {
        if (m_engine->isRunning()) {
            finishTracking(QStringLiteral("resumed"));
        } else if (m_engine->isIdle()) {
            finishTracking(QStringLiteral("stopped"));
        }
    });
}

void SessionKeeper::captureTimer()
{
    const TimerPreset preset = m_presets ? m_presets->currentPreset() : TimerPreset();
    m_timerUuid = preset.uuid;
    m_timerName = preset.name;
    m_timerCategory = preset.category;
}

void SessionKeeper::save()
{
    if (!m_engine || m_quitting || !m_restoreDone || m_path.isEmpty()) {
        return; // before restore(), the file still holds the last run's session
    }
    write();
}

bool SessionKeeper::write()
{
    if (!m_engine->isActive() || m_engine->continuesEndedSession()) {
        m_heartbeat.stop();
        removeFile();
        return false;
    }
    // Also while paused: `savedAtMs` says Sugo was still running then (the age limit counts from it).
    if (!m_heartbeat.isActive()) {
        m_heartbeat.start();
    }

    const QJsonObject state{
        {QStringLiteral("format"), kFormat},
        {QStringLiteral("presetUuid"), m_timerUuid},
        {QStringLiteral("presetName"), m_timerName},
        {QStringLiteral("category"), m_timerCategory},
        {QStringLiteral("phase"), phaseName(m_engine->phase())},
        {QStringLiteral("plannedMs"), m_engine->plannedMs()},
        {QStringLiteral("remainingMs"), m_engine->remainingMs()},
        {QStringLiteral("startedAtMs"), m_engine->phaseStartedAt().toMSecsSinceEpoch()},
        {QStringLiteral("cycles"), m_engine->completedCycles()},
        {QStringLiteral("savedAtMs"), m_now().toMSecsSinceEpoch()},
    };
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(state).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        qCWarning(lcSessionKeeper) << "Cannot keep the timer's session in" << m_path << ":" << file.errorString();
        return false;
    }
    return true;
}

bool SessionKeeper::keepForQuit()
{
    if (!m_engine || !m_restoreDone || m_path.isEmpty()) {
        return false;
    }
    // A phase whose time ran out before the next tick has ended: it is recorded as completed, not
    // kept with no time left (the next start would drop it).
    m_engine->refresh();
    if (!m_engine->isActive() || m_engine->continuesEndedSession()) {
        return false;
    }
    m_heartbeat.stop();
    const bool kept = write();
    if (!kept) {
        // An older copy would bring back a stale time next start, and the timer now records the
        // session as interrupted: there must be no file left.
        removeFile();
    }
    m_quitting = true; // what the quit does to the timer next must not touch the file
    return kept;
}

bool SessionKeeper::restore()
{
    if (!m_engine || m_path.isEmpty() || m_restoreDone) {
        return false;
    }
    m_restoreDone = true;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    removeFile(); // brought back once only

    TimerEngine::Phase phase = TimerEngine::Phase::Work;
    const qint64 plannedMs = state.value(QStringLiteral("plannedMs")).toInteger(-1);
    const qint64 remainingMs = state.value(QStringLiteral("remainingMs")).toInteger(-1);
    const qint64 startedAtMs = state.value(QStringLiteral("startedAtMs")).toInteger(0);
    const qint64 savedAtMs = state.value(QStringLiteral("savedAtMs")).toInteger(0);
    const int cycles = state.value(QStringLiteral("cycles")).toInt(0);
    const QString uuid = state.value(QStringLiteral("presetUuid")).toString();
    if (state.value(QStringLiteral("format")).toInt() != kFormat
        || !phaseFromName(state.value(QStringLiteral("phase")).toString(), &phase) || plannedMs <= 0 || remainingMs <= 0
        || remainingMs > plannedMs || startedAtMs <= 0 || savedAtMs < startedAtMs) {
        qCWarning(lcSessionKeeper) << "The session kept in" << m_path << "cannot be read; it is dropped";
        return false;
    }

    const qint64 now = m_now().toMSecsSinceEpoch();
    const bool timerKnown = m_presets && !uuid.isEmpty() && m_presets->indexOf(uuid) >= 0;
    if (now - savedAtMs > kMaxAgeMs || now < startedAtMs || !timerKnown || !m_engine->isIdle()) {
        // Ended where the last run left it, as stopping it then would have recorded it.
        if (m_recorder) {
            SessionRecord record;
            record.kind = kindFor(phase);
            record.startedAtMs = startedAtMs;
            record.endedAtMs = savedAtMs;
            record.durationSec = int((plannedMs - remainingMs + 500) / 1000);
            record.plannedSec = int((plannedMs + 500) / 1000);
            record.completed = false;
            record.presetUuid = uuid;
            record.presetName = state.value(QStringLiteral("presetName")).toString();
            record.category = state.value(QStringLiteral("category")).toString();
            m_recorder->storeEnded(record, plannedMs - remainingMs);
        }
        Q_EMIT restoreEnded(phase, QStringLiteral("expired"));
        return false;
    }

    if (m_presets->currentUuid() != uuid) {
        m_presets->setCurrentUuid(uuid); // resets the idle timer to that timer's durations
    }
    if (!m_engine->restorePaused(phase, plannedMs, remainingMs, QDateTime::fromMSecsSinceEpoch(startedAtMs, QTimeZone::UTC),
                                 cycles)) {
        return false;
    }
    // The session keeps the timer name and category it began with.
    m_timerUuid = uuid;
    m_timerName = state.value(QStringLiteral("presetName")).toString();
    m_timerCategory = state.value(QStringLiteral("category")).toString();
    if (m_recorder) {
        m_recorder->setActiveTimer(m_timerName, m_timerCategory);
    }
    m_tracking = true;
    m_trackedPhase = phase;
    setNotice(true);
    save();
    return true;
}

void SessionKeeper::dismiss()
{
    setNotice(false);
}

void SessionKeeper::finishTracking(const QString &result)
{
    if (!m_tracking) {
        return;
    }
    m_tracking = false;
    setNotice(false);
    Q_EMIT restoreEnded(m_trackedPhase, result);
}

void SessionKeeper::setNotice(bool shown)
{
    if (m_notice != shown) {
        m_notice = shown;
        Q_EMIT restoredChanged();
    }
}

void SessionKeeper::removeFile()
{
    if (QFile::exists(m_path) && !QFile::remove(m_path)) {
        qCWarning(lcSessionKeeper) << "Cannot remove" << m_path;
    }
}

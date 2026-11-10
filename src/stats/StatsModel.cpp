// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatsModel.h"

#include <algorithm>

#include "Localization.h"

#include "SessionRecorder.h"

namespace
{
StatsPeriod toAggregatorPeriod(StatsModel::Period p)
{
    switch (p) {
    case StatsModel::Period::Day:
        return StatsPeriod::Day;
    case StatsModel::Period::Week:
        return StatsPeriod::Week;
    case StatsModel::Period::Month:
        return StatsPeriod::Month;
    case StatsModel::Period::Year:
        return StatsPeriod::Year;
    }
    Q_UNREACHABLE_RETURN(StatsPeriod::Week);
}

/// The periodic safety net behind the midnight timer: a suspended machine or a changed clock
/// makes a single-shot timer fire late (or never), this catches up within a few minutes.
constexpr int kDateCheckIntervalMs = 5 * 60 * 1000;

QString capitalized(QString s)
{
    if (!s.isEmpty()) {
        s[0] = s[0].toUpper();
    }
    return s;
}
} // namespace

StatsModel::StatsModel(QObject *parent)
    : QObject(parent)
{
    m_env.followSystem = true;
    readSystemEnvironment();
    m_today = todayDate();
    m_anchorDate = m_today;

    m_midnightTimer.setSingleShot(true);
    m_midnightTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_midnightTimer, &QTimer::timeout, this, &StatsModel::checkDateChange);
    m_dateCheckTimer.setInterval(kDateCheckIntervalMs);
    m_dateCheckTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_dateCheckTimer, &QTimer::timeout, this, &StatsModel::checkDateChange);
    m_dateCheckTimer.start();
    armMidnightTimer();
    // The notes follow everything the period's figures follow.
    connect(this, &StatsModel::statsChanged, this, &StatsModel::rebuildNotes);
    connect(this, &StatsModel::statsChanged, this, &StatsModel::rebuildSessions);
}

void StatsModel::attach(SessionRepository *repository, SessionRecorder *recorder)
{
    m_repository = repository;
    if (recorder) {
        connect(recorder, &SessionRecorder::sessionRecorded, this, &StatsModel::onSessionRecorded, Qt::UniqueConnection);
    }
    refresh();
}

void StatsModel::setEnvironment(const Environment &env)
{
    m_env = env;
    if (m_env.followSystem) {
        readSystemEnvironment();
    }
    m_today = todayDate();
    m_anchorDate = m_today;
    armMidnightTimer();
    refresh();
    Q_EMIT periodChanged();
}

QDate StatsModel::todayDate() const
{
    return m_env.now().toTimeZone(m_env.timeZone).date();
}

void StatsModel::readSystemEnvironment()
{
    m_env.timeZone = QTimeZone::systemTimeZone();
    m_env.locale = QLocale();
    m_env.firstDayOfWeek = m_env.locale.firstDayOfWeek();
}

void StatsModel::armMidnightTimer()
{
    const QDateTime now = m_env.now().toTimeZone(m_env.timeZone);
    const QDateTime nextMidnight = now.date().addDays(1).startOfDay(m_env.timeZone);
    // A little past midnight, so the new day has surely begun; never a busy loop, and never
    // longer than a day (the periodic check covers anything this misses).
    const qint64 ms = now.msecsTo(nextMidnight) + 1000;
    m_midnightTimer.start(int(qBound<qint64>(1000, ms, 24 * 3600 * 1000LL + 1000)));
}

void StatsModel::checkDateChange()
{
    const QTimeZone oldZone = m_env.timeZone;
    const QLocale oldLocale = m_env.locale;
    const Qt::DayOfWeek oldFirstDay = m_env.firstDayOfWeek;
    if (m_env.followSystem) {
        readSystemEnvironment();
    }
    const bool zoneChanged = m_env.timeZone != oldZone;
    const bool localeChanged = m_env.locale != oldLocale || m_env.firstDayOfWeek != oldFirstDay;
    const QDate newToday = todayDate();
    armMidnightTimer();
    if (newToday == m_today && !zoneChanged && !localeChanged) {
        // Same day, but a period under way still moves on: the bar of "now" and the point of the
        // period before that it is compared with.
        if (isCurrent()) {
            recompute();
        }
        return;
    }

    const QDate oldToday = m_today;
    m_today = newToday;
    // Follow along if the shown period was the one containing the old today.
    const StatsPeriod sp = toStatsPeriod(m_period);
    const PeriodRange shown = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);
    if (oldToday >= shown.firstDay && oldToday <= shown.lastDay) {
        m_anchorDate = newToday;
    }
    if (zoneChanged) {
        rebuildCompletedDays();
    }
    recompute();
    recomputeStreaks();
    recomputeToday();
    Q_EMIT periodChanged();
}

StatsPeriod StatsModel::toStatsPeriod(Period p) const
{
    return toAggregatorPeriod(p);
}

void StatsModel::setPeriod(Period period)
{
    if (m_period == period) {
        return;
    }
    m_period = period;
    recompute();
    Q_EMIT periodChanged();
}

void StatsModel::setBreakdownMode(BreakdownMode mode)
{
    if (m_breakdownMode == mode) {
        return;
    }
    m_breakdownMode = mode;
    Q_EMIT breakdownModeChanged();
    Q_EMIT breakdownChanged();
}

QString StatsModel::title() const
{
    const QLocale &locale = m_env.locale;
    const StatsPeriod sp = toStatsPeriod(m_period);
    const PeriodRange range = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);

    switch (m_period) {
    case Period::Day:
        return capitalized(locale.toString(m_anchorDate, QLocale::LongFormat));

    case Period::Week: {
        QString first;
        QString last;
        if (range.firstDay.year() == range.lastDay.year()) {
            if (range.firstDay.month() == range.lastDay.month()) {
                first = locale.toString(range.firstDay,
                                        i18nc("QDate::toString() format of the first day of a week within one month, "
                                              "e.g. \"5\" in \"5 – 11 October 2026\"",
                                              "d"));
                last = locale.toString(range.lastDay,
                                       i18nc("QDate::toString() format of the last day of a week within one month, "
                                             "e.g. \"11 October 2026\" in \"5 – 11 October 2026\"",
                                             "d MMMM yyyy"));
            } else {
                first = locale.toString(range.firstDay,
                                        i18nc("QDate::toString() format of the first day of a week spanning two months, "
                                              "e.g. \"28 Sep\" in \"28 Sep – 4 Oct 2026\"",
                                              "d MMM"));
                last = locale.toString(range.lastDay,
                                       i18nc("QDate::toString() format of the last day of a week spanning two months, "
                                             "e.g. \"4 Oct 2026\" in \"28 Sep – 4 Oct 2026\"",
                                             "d MMM yyyy"));
            }
        } else {
            const QString format = i18nc("QDate::toString() format of either day of a week spanning two years, "
                                         "e.g. \"29 Dec 2025\"",
                                         "d MMM yyyy");
            first = locale.toString(range.firstDay, format);
            last = locale.toString(range.lastDay, format);
        }
        return capitalized(i18nc("@title week range: %1 first day, %2 last day", "%1 – %2", first, last));
    }

    case Period::Month:
        return capitalized(i18nc("@title %1 month name, %2 year", "%1 %2",
                                 locale.standaloneMonthName(m_anchorDate.month(), QLocale::LongFormat),
                                 QString::number(m_anchorDate.year())));

    case Period::Year:
        return QString::number(m_anchorDate.year());
    }

    return QString();
}

bool StatsModel::isCurrent() const
{
    const qint64 nowMs = m_env.now().toMSecsSinceEpoch();
    const PeriodRange range = Stats::rangeFor(toStatsPeriod(m_period), m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);
    return range.startMs <= nowMs && nowMs < range.endMs;
}

bool StatsModel::canGoNext() const
{
    const qint64 nowMs = m_env.now().toMSecsSinceEpoch();
    const QDate nextAnchor = Stats::shifted(toStatsPeriod(m_period), m_anchorDate, 1);
    const PeriodRange nextRange = Stats::rangeFor(toStatsPeriod(m_period), nextAnchor, m_env.firstDayOfWeek, m_env.timeZone);
    return nextRange.startMs <= nowMs;
}

void StatsModel::previousPeriod()
{
    moveAnchor(-1);
}

void StatsModel::nextPeriod()
{
    if (!canGoNext()) {
        return;
    }
    moveAnchor(1);
}

void StatsModel::moveAnchor(int steps)
{
    const StatsPeriod sp = toStatsPeriod(m_period);
    m_anchorDate = Stats::shifted(sp, m_anchorDate, steps);
    // Months and years clamp the day (31 March - 1 month = 28 February), so coming back would
    // land on 28 March: the period under way is anchored on today, as it was before.
    const QDate current = todayDate();
    const PeriodRange range = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);
    if (current >= range.firstDay && current <= range.lastDay) {
        m_anchorDate = current;
    }
    recompute();
    Q_EMIT periodChanged();
}

void StatsModel::today()
{
    const QDate current = todayDate();
    if (m_anchorDate == current) {
        return;
    }
    m_anchorDate = current;
    recompute();
    Q_EMIT periodChanged();
}

void StatsModel::refresh()
{
    // Full reload: history may have been cleared, restored or edited.
    reloadCompletedStarts();
    recompute();
    recomputeStreaks();
    recomputeToday();
}

void StatsModel::onSessionRecorded()
{
    // A new row only: extend the cache instead of reloading the whole history.
    loadNewCompletedStarts();
    recompute();
    recomputeStreaks();
    recomputeToday();
}

void StatsModel::reloadCompletedStarts()
{
    m_completedStarts.clear();
    m_completedStartsMaxId = 0;
    loadNewCompletedStarts();
    rebuildCompletedDays();
}

void StatsModel::loadNewCompletedStarts()
{
    if (!m_repository) {
        return;
    }
    const QList<qint64> added = m_repository->completedWorkStartsAfter(m_completedStartsMaxId, &m_completedStartsMaxId);
    m_completedStarts.append(added);
    for (const qint64 ms : added) {
        ++m_completedPerDay[QDateTime::fromMSecsSinceEpoch(ms, m_env.timeZone).date()];
    }
}

void StatsModel::rebuildCompletedDays()
{
    m_completedPerDay.clear();
    for (const qint64 ms : std::as_const(m_completedStarts)) {
        ++m_completedPerDay[QDateTime::fromMSecsSinceEpoch(ms, m_env.timeZone).date()];
    }
}

void StatsModel::recompute()
{
    if (!m_repository) {
        m_summary = StatsSummary();
        m_previousWorkSeconds = 0;
        m_currentRecords.clear();
        m_completedStarts.clear();
        m_completedStartsMaxId = 0;
        m_completedPerDay.clear();
        m_bucketsVariant.clear();
        m_daysVariant.clear();
        Q_EMIT statsChanged();
        Q_EMIT breakdownChanged();
        return;
    }

    const StatsPeriod sp = toStatsPeriod(m_period);
    const PeriodRange range = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);

    m_currentRecords = m_repository->between(range.startMs, range.endMs);
    m_summary = Stats::summarize(m_currentRecords, sp, m_anchorDate, m_env.firstDayOfWeek,
                                 m_env.timeZone, m_env.locale, m_env.now());

    // The period before, up to the same point when the shown one is still under way.
    {
        const QDate previousAnchor = Stats::shifted(sp, m_anchorDate, -1);
        const PeriodRange previous = Stats::rangeFor(sp, previousAnchor, m_env.firstDayOfWeek, m_env.timeZone);
        const QDateTime now = m_env.now();
        const qint64 nowMs = now.toMSecsSinceEpoch();
        qint64 endMs = previous.endMs;
        if (range.startMs <= nowMs && nowMs < range.endMs) {
            endMs = Stats::samePointInPreviousPeriod(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone, now);
        }
        m_previousWorkSeconds = 0;
        const QList<SessionRecord> records = m_repository->between(previous.startMs, endMs);
        for (const SessionRecord &record : records) {
            if (record.kind == SessionKind::Work
                && record.startedAtMs >= previous.startMs && record.startedAtMs < endMs) {
                m_previousWorkSeconds += record.durationSec;
            }
        }
    }

    m_bucketsVariant.clear();
    qint64 hourStartMs = range.startMs; // day view: one bar per real hour

    for (const StatsBucket &b : m_summary.buckets) {

        QString tooltip;
        switch (m_period) {
        case Period::Day:
            if (b.hour == b.endHour) {
                // The hour repeated when the clocks go back: only the UTC offsets tell its ends apart.
                const QDateTime start = QDateTime::fromMSecsSinceEpoch(hourStartMs, m_env.timeZone);
                const QDateTime end = QDateTime::fromMSecsSinceEpoch(hourStartMs + 3600 * 1000, m_env.timeZone);
                tooltip = i18n("%1:00 %2 – %3:00 %4\nWork: %5\nBreaks: %6",
                               b.hour, m_env.timeZone.displayName(start, QTimeZone::OffsetName),
                               b.endHour, m_env.timeZone.displayName(end, QTimeZone::OffsetName),
                               formatDuration(b.workSeconds),
                               formatDuration(b.breakSeconds));
            } else {
                tooltip = i18n("%1:00 – %2:00\nWork: %3\nBreaks: %4",
                               b.hour, b.endHour,
                               formatDuration(b.workSeconds),
                               formatDuration(b.breakSeconds));
            }
            hourStartMs += 3600 * 1000;
            break;
        case Period::Week:
        case Period::Month:
            tooltip = i18n("%1\nWork: %2\nBreaks: %3",
                           m_env.locale.toString(b.date, QLocale::LongFormat),
                           formatDuration(b.workSeconds),
                           formatDuration(b.breakSeconds));
            break;
        case Period::Year:
            tooltip = i18n("%1\nWork: %2\nBreaks: %3",
                           m_env.locale.standaloneMonthName(b.date.month(), QLocale::LongFormat),
                           formatDuration(b.workSeconds),
                           formatDuration(b.breakSeconds));
            break;
        }

        QVariantMap map;
        map[QStringLiteral("label")] = b.label;
        map[QStringLiteral("workSeconds")] = b.workSeconds;
        map[QStringLiteral("breakSeconds")] = b.breakSeconds;
        map[QStringLiteral("totalSeconds")] = b.workSeconds + b.breakSeconds;
        map[QStringLiteral("isCurrent")] = b.isCurrent;
        map[QStringLiteral("tooltip")] = tooltip;
        map[QStringLiteral("hour")] = b.hour;
        map[QStringLiteral("date")] = b.date;
        // How the bucket is named on its own ("14:00", "Friday", "30 October", "October").
        switch (m_period) {
        case Period::Day:
            map[QStringLiteral("name")] = i18nc("clock hour", "%1:00", b.hour);
            break;
        case Period::Week:
            map[QStringLiteral("name")] = m_env.locale.standaloneDayName(b.date.dayOfWeek(), QLocale::LongFormat);
            break;
        case Period::Month:
            map[QStringLiteral("name")] = m_env.locale.toString(
                b.date, i18nc("QDate::toString() format of a day of the month, e.g. \"30 October\"", "d MMMM"));
            break;
        case Period::Year:
            map[QStringLiteral("name")] = m_env.locale.standaloneMonthName(b.date.month(), QLocale::LongFormat);
            break;
        }

        m_bucketsVariant.append(map);
    }


    // Work time per day, filtered like the summary, for the year's map of days.
    QHash<QDate, qint64> workPerDay;
    for (const SessionRecord &record : std::as_const(m_currentRecords)) {
        if (record.kind != SessionKind::Work
            || record.startedAtMs < range.startMs || record.startedAtMs >= range.endMs) {
            continue;
        }
        workPerDay[QDateTime::fromMSecsSinceEpoch(record.startedAtMs, m_env.timeZone).date()] += record.durationSec;
    }
    m_daysVariant.clear();
    const QDate today = todayDate();
    for (QDate day = range.firstDay; day <= range.lastDay; day = day.addDays(1)) {
        const qint64 seconds = workPerDay.value(day, 0);
        QVariantMap map;
        map[QStringLiteral("date")] = day;
        // Plain numbers: a QDate turned into a JavaScript Date can land on the day before.
        map[QStringLiteral("weekday")] = day.dayOfWeek() % 7; // 0 = Sunday, as Qt.locale().firstDayOfWeek
        map[QStringLiteral("dayOfMonth")] = day.day();
        map[QStringLiteral("month")] = day.month();
        map[QStringLiteral("workSeconds")] = seconds;
        map[QStringLiteral("isToday")] = day == today;
        map[QStringLiteral("tooltip")] = i18n("%1\nWork: %2", m_env.locale.toString(day, QLocale::LongFormat),
                                              formatDuration(seconds));
        m_daysVariant.append(map);
    }
    Q_EMIT statsChanged();
    Q_EMIT breakdownChanged();
}

void StatsModel::recomputeStreaks()
{
    if (!m_repository) {
        m_currentStreak = 0;
        m_bestStreak = 0;
        Q_EMIT streaksChanged();
        return;
    }

    // Works on the per-day cache: proportional to the number of active days, not sessions.
    QSet<QDate> days;
    days.reserve(m_completedPerDay.size());
    for (auto it = m_completedPerDay.cbegin(); it != m_completedPerDay.cend(); ++it) {
        days.insert(it.key());
    }
    // m_today is deliberately left alone: checkDateChange() compares against it to notice that the
    // day rolled over and to move the shown period, so updating it here would hide the change.
    const QDate today = todayDate();

    m_currentStreak = Stats::currentStreak(days, today, m_protectWeekendStreak);
    m_bestStreak = Stats::bestStreak(days, m_protectWeekendStreak);

    // Only the streaks: every statsChanged property comes from recompute().
    Q_EMIT streaksChanged();
}

void StatsModel::setProtectWeekendStreak(bool protect)
{
    if (m_protectWeekendStreak == protect) {
        return;
    }
    m_protectWeekendStreak = protect;
    recomputeStreaks();
    Q_EMIT protectWeekendStreakChanged();
}

QVariantList StatsModel::breakdown() const
{
    QVariantList list;
    const QList<BreakdownEntry> *entriesPtr = nullptr;
    if (m_breakdownMode == BreakdownMode::ByTimer) {
        entriesPtr = &m_summary.byTimer;
    } else if (m_breakdownMode == BreakdownMode::ByCategory) {
        entriesPtr = &m_summary.byCategory;
    } else {
        entriesPtr = &m_summary.byTask;
    }
    const QList<BreakdownEntry> &entries = *entriesPtr;

    const qint64 totalWork = m_summary.workSeconds;

    for (const BreakdownEntry &entry : entries) {
        QVariantMap map;
        QString name = entry.name;
        if (name.isEmpty()) {
            if (m_breakdownMode == BreakdownMode::ByTimer) {
                name = i18n("Untitled Timer");
            } else if (m_breakdownMode == BreakdownMode::ByCategory) {
                name = i18n("Uncategorized");
            } else {
                name = i18n("Without note");
            }
        }
        map[QStringLiteral("name")] = name;
        map[QStringLiteral("workSeconds")] = entry.workSeconds;
        map[QStringLiteral("sessions")] = entry.sessions;
        map[QStringLiteral("formattedDuration")] = formatDuration(entry.workSeconds);

        const double pct = totalWork > 0 ? (double(entry.workSeconds) / double(totalWork)) * 100.0 : 0.0;
        map[QStringLiteral("percentage")] = pct;
        map[QStringLiteral("formattedPercentage")] = QStringLiteral("%1%").arg(QString::number(pct, 'f', 1));

        list.append(map);
    }

    return list;
}

QString StatsModel::formatDuration(qint64 seconds) const
{
    if (seconds <= 0) {
        return i18n("0m");
    }
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;

    if (hours > 0) {
        if (minutes > 0) {
            return i18n("%1h %2m", hours, minutes);
        }
        return i18n("%1h", hours);
    }
    if (minutes > 0) {
        return i18n("%1m", minutes);
    }
    return i18n("%1s", secs);
}


// --- Today, whatever period is shown ---------------------------------------------------------

void StatsModel::recomputeToday()
{
    qint64 workSeconds = 0;
    int completed = 0;
    if (m_repository) {
        // The real today (not m_today, which checkDateChange() compares against to notice a new
        // day), as recomputeStreaks() does.
        const QDate today = todayDate();
        const PeriodRange range = Stats::rangeFor(StatsPeriod::Day, today, m_env.firstDayOfWeek, m_env.timeZone);
        const StatsSummary summary = Stats::summarize(m_repository->between(range.startMs, range.endMs), StatsPeriod::Day,
                                                      today, m_env.firstDayOfWeek, m_env.timeZone, m_env.locale, m_env.now());
        workSeconds = summary.workSeconds;
        completed = summary.completedWorkSessions;
    }
    if (workSeconds != m_todayWorkSeconds || completed != m_todayCompletedSessions) {
        m_todayWorkSeconds = workSeconds;
        m_todayCompletedSessions = completed;
        Q_EMIT todayChanged();
    }
}

// --- The task notes of the shown period ------------------------------------------------------

void StatsModel::rebuildNotes()
{
    const StatsPeriod sp = toStatsPeriod(m_period);
    const PeriodRange range = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);

    QList<const SessionRecord *> withNotes;
    for (const SessionRecord &record : std::as_const(m_currentRecords)) {
        if (record.kind == SessionKind::Work && record.startedAtMs >= range.startMs
            && record.startedAtMs < range.endMs && !record.note.trimmed().isEmpty()) {
            withNotes.append(&record);
        }
    }
    std::sort(withNotes.begin(), withNotes.end(), [](const SessionRecord *a, const SessionRecord *b) {
        return a->startedAtMs != b->startedAtMs ? a->startedAtMs > b->startedAtMs : a->id > b->id;
    });

    QVariantList notes;
    for (const SessionRecord *record : std::as_const(withNotes)) {
        if (notes.size() >= kMaxNotes) {
            break;
        }
        QString label;
        QString when;
        describeStart(record->startedAtMs, &label, &when);
        QVariantMap map;
        map[QStringLiteral("sessionId")] = record->id;
        map[QStringLiteral("note")] = record->note.trimmed();
        map[QStringLiteral("time")] = QDateTime::fromMSecsSinceEpoch(record->startedAtMs, m_env.timeZone);
        map[QStringLiteral("label")] = label;
        map[QStringLiteral("when")] = when;
        notes.append(map);
    }

    if (notes != m_notesVariant) {
        m_notesVariant = notes;
        Q_EMIT notesChanged();
    }
}

void StatsModel::describeStart(qint64 startedAtMs, QString *label, QString *when) const
{
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(startedAtMs, m_env.timeZone);
    const QString time = m_env.locale.toString(start.time(), QLocale::ShortFormat);
    if (m_period == Period::Day || start.date() == todayDate()) {
        *label = time;
    } else if (m_period == Period::Week) {
        *label = m_env.locale.dayName(start.date().dayOfWeek(), QLocale::ShortFormat);
    } else {
        *label = m_env.locale.toString(
            start.date(), i18nc("QDate::toString() format of the day of a task note, e.g. \"5 Oct\"", "d MMM"));
    }
    *when = i18nc("when a task note was written: date, time", "%1, %2",
                  m_env.locale.toString(start.date(), QLocale::LongFormat), time);
}

// --- Every session of the shown period ---------------------------------------------------------

void StatsModel::rebuildSessions()
{
    const StatsPeriod sp = toStatsPeriod(m_period);
    const PeriodRange range = Stats::rangeFor(sp, m_anchorDate, m_env.firstDayOfWeek, m_env.timeZone);

    QList<const SessionRecord *> shown;
    for (const SessionRecord &record : std::as_const(m_currentRecords)) {
        if (record.startedAtMs >= range.startMs && record.startedAtMs < range.endMs) {
            shown.append(&record);
        }
    }
    std::sort(shown.begin(), shown.end(), [](const SessionRecord *a, const SessionRecord *b) {
        return a->startedAtMs != b->startedAtMs ? a->startedAtMs > b->startedAtMs : a->id > b->id;
    });

    QVariantList sessions;
    for (const SessionRecord *record : std::as_const(shown)) {
        QString label;
        QString when;
        describeStart(record->startedAtMs, &label, &when);
        QString kindName;
        switch (record->kind) {
        case SessionKind::Work:
            kindName = i18nc("a kind of session", "Focus");
            break;
        case SessionKind::ShortBreak:
            kindName = i18nc("a kind of session", "Short break");
            break;
        case SessionKind::LongBreak:
            kindName = i18nc("a kind of session", "Long break");
            break;
        }
        QVariantMap map;
        map[QStringLiteral("sessionId")] = record->id;
        map[QStringLiteral("kind")] = SessionRepository::kindToString(record->kind);
        map[QStringLiteral("kindName")] = kindName;
        map[QStringLiteral("timer")] = record->presetName.isEmpty() ? i18n("Untitled Timer") : record->presetName;
        map[QStringLiteral("note")] = record->note.trimmed();
        map[QStringLiteral("completed")] = record->completed;
        map[QStringLiteral("duration")] = formatDuration(record->durationSec);
        map[QStringLiteral("label")] = label;
        map[QStringLiteral("when")] = when;
        sessions.append(map);
    }

    if (sessions != m_sessionsVariant) {
        m_sessionsVariant = sessions;
        Q_EMIT sessionsChanged();
    }
}

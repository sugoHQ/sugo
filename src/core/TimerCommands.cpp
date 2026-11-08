// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimerCommands.h"

#include "Localization.h"
#include "PresetModel.h"
#include "TimerEngine.h"

#include <QUrl>

TimerCommands::TimerCommands(TimerEngine *engine, PresetModel *presets)
    : m_engine(engine)
    , m_presets(presets)
{
}

bool TimerCommands::mayControl(Result &result) const
{
    if (!m_engine) {
        result = {Error::Unavailable, {}};
        return false;
    }
    if (!m_allowed || m_allowed()) {
        return true;
    }
    result = {Error::ControlDisabled, i18n("Control by other programs is turned off in Sugo's settings.")};
    return false;
}

template<typename Action>
TimerCommands::Result TimerCommands::simple(Action action)
{
    Result result;
    if (mayControl(result)) {
        action(m_engine);
    }
    return result;
}

TimerCommands::Result TimerCommands::toggle()
{
    return simple([](TimerEngine *engine) { engine->toggle(); });
}

TimerCommands::Result TimerCommands::start()
{
    return simple([](TimerEngine *engine) {
        if (engine->isPaused()) {
            engine->resume();
        } else {
            engine->start();
        }
    });
}

TimerCommands::Result TimerCommands::pause()
{
    return simple([](TimerEngine *engine) { engine->pause(); });
}

TimerCommands::Result TimerCommands::resume()
{
    return simple([](TimerEngine *engine) { engine->resume(); });
}

TimerCommands::Result TimerCommands::stop()
{
    return simple([](TimerEngine *engine) { engine->stop(); });
}

TimerCommands::Result TimerCommands::skip()
{
    return simple([](TimerEngine *engine) { engine->skip(); });
}

QString TimerCommands::resolvePreset(const QString &nameOrUuid) const
{
    if (!m_presets) {
        return {};
    }
    const QList<TimerPreset> &list = m_presets->presets();
    for (const TimerPreset &preset : list) {
        if (preset.uuid == nameOrUuid) {
            return preset.uuid;
        }
    }
    // Names are stored trimmed; a name typed or pasted with spaces around it still matches.
    const QString name = nameOrUuid.trimmed();
    for (const TimerPreset &preset : list) {
        if (preset.name.compare(name, Qt::CaseInsensitive) == 0) {
            return preset.uuid;
        }
    }
    return {};
}

TimerCommands::Result TimerCommands::switchTo(const QString &uuid)
{
    if (uuid == m_presets->currentUuid()) {
        return {};
    }
    if (m_engine->isActive()) {
        return {Error::TimerActive, i18n("A timer is running. Stop it before choosing another timer.")};
    }
    m_presets->setCurrentUuid(uuid);
    return {};
}

TimerCommands::Result TimerCommands::selectPreset(const QString &nameOrUuid)
{
    Result result;
    if (!mayControl(result)) {
        return result;
    }
    const QString uuid = resolvePreset(nameOrUuid);
    if (uuid.isEmpty()) {
        return {Error::UnknownPreset, i18n("There is no timer named \"%1\".", nameOrUuid)};
    }
    return switchTo(uuid);
}

TimerCommands::Result TimerCommands::stepPreset(int step)
{
    Result result;
    if (!mayControl(result) || !m_presets || m_presets->count() == 0) {
        return result;
    }
    const int count = m_presets->count();
    const int row = ((m_presets->currentIndex() + step) % count + count) % count;
    return switchTo(m_presets->uuidAt(row));
}

TimerCommands::Result TimerCommands::startPreset(const QString &nameOrUuid)
{
    Result result = selectPreset(nameOrUuid);
    if (!result.ok()) {
        return result;
    }
    if (m_engine->isPaused()) {
        m_engine->resume();
    } else {
        m_engine->start();
    }
    return result;
}

QStringList TimerCommands::presets() const
{
    QStringList names;
    if (m_presets) {
        for (const TimerPreset &preset : m_presets->presets()) {
            names.append(preset.name);
        }
    }
    return names;
}

QString TimerCommands::status() const
{
    if (!m_engine) {
        return QStringLiteral("unavailable");
    }
    QString stateStr;
    if (m_engine->state() == TimerEngine::State::Idle) {
        stateStr = QStringLiteral("idle");
    } else if (m_engine->isPaused()) {
        stateStr = QStringLiteral("paused");
    } else {
        stateStr = QStringLiteral("running");
    }

    QString phaseStr;
    switch (m_engine->phase()) {
    case TimerEngine::Phase::Work:
        phaseStr = QStringLiteral("work");
        break;
    case TimerEngine::Phase::ShortBreak:
        phaseStr = QStringLiteral("short_break");
        break;
    case TimerEngine::Phase::LongBreak:
        phaseStr = QStringLiteral("long_break");
        break;
    }

    return QStringLiteral("%1:%2:%3").arg(stateStr, phaseStr, QString::number(m_engine->remainingSeconds()));
}

TimerCommands::UrlCommand TimerCommands::parseUrl(const QUrl &url)
{
    using Action = UrlCommand::Action;
    UrlCommand command;
    if (!url.isValid() || url.scheme().compare(QLatin1String("sugo"), Qt::CaseInsensitive) != 0) {
        return command;
    }

    // sugo://toggle has the command as its host, sugo:toggle (and sugo:///toggle) in the path.
    QString path = url.path(QUrl::FullyDecoded);
    QString name = url.host(QUrl::FullyDecoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (name.isEmpty()) {
        const qsizetype slash = path.indexOf(QLatin1Char('/'));
        name = slash < 0 ? path : path.left(slash);
        path = slash < 0 ? QString() : path.mid(slash + 1);
    }
    name = name.toLower();
    const QString argument = path.trimmed();

    static const QList<std::pair<QLatin1String, Action>> simpleCommands{
        {QLatin1String("start"), Action::Start},   {QLatin1String("pause"), Action::Pause},
        {QLatin1String("resume"), Action::Resume}, {QLatin1String("toggle"), Action::Toggle},
        {QLatin1String("stop"), Action::Stop},     {QLatin1String("skip"), Action::Skip},
        {QLatin1String("show"), Action::Show},
    };
    if (name == QLatin1String("preset")) {
        if (!argument.isEmpty()) {
            command = {Action::SelectPreset, argument};
        }
        return command;
    }
    for (const auto &[word, action] : simpleCommands) {
        if (name == word) {
            // Only start takes an argument (the timer to start).
            if (argument.isEmpty() || action == Action::Start) {
                command = {action, argument};
            }
            return command;
        }
    }
    return command;
}

TimerCommands::Result TimerCommands::run(const UrlCommand &command)
{
    using Action = UrlCommand::Action;
    switch (command.action) {
    case Action::Invalid:
        break;
    case Action::Start:
        return command.argument.isEmpty() ? start() : startPreset(command.argument);
    case Action::Pause:
        return pause();
    case Action::Resume:
        return resume();
    case Action::Toggle:
        return toggle();
    case Action::Stop:
        return stop();
    case Action::Skip:
        return skip();
    case Action::Show:
        return {};
    case Action::SelectPreset:
        return selectPreset(command.argument);
    }
    return {Error::UnknownCommand, i18n("Sugo does not know this link.")};
}

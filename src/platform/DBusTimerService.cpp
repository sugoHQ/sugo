// SPDX-License-Identifier: GPL-3.0-or-later
#include "DBusTimerService.h"

DBusTimerService::DBusTimerService(TimerEngine *engine, PresetModel *presets, TimerDBusObject *host)
    : QDBusAbstractAdaptor(host)
    , m_commands(engine, presets)
{
}

bool DBusTimerService::answer(const TimerCommands::Result &result)
{
    QString name;
    switch (result.error) {
    case TimerCommands::Error::None:
        return true;
    case TimerCommands::Error::Unavailable:
        return false; // nothing to control: no answer, as before there was an engine
    case TimerCommands::Error::ControlDisabled:
        name = ControlDisabledError;
        break;
    case TimerCommands::Error::UnknownPreset:
        name = UnknownPresetError;
        break;
    case TimerCommands::Error::TimerActive:
        name = TimerActiveError;
        break;
    case TimerCommands::Error::UnknownCommand:
        name = QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod");
        break;
    }
    auto *host = static_cast<TimerDBusObject *>(parent());
    if (host->calledFromDBus()) {
        host->sendErrorReply(name, result.message);
    }
    return false;
}

QStringList DBusTimerService::presets() const
{
    return m_commands.presets();
}

bool DBusTimerService::selectPreset(const QString &nameOrUuid)
{
    return answer(m_commands.selectPreset(nameOrUuid));
}

void DBusTimerService::nextPreset()
{
    answer(m_commands.stepPreset(1));
}

void DBusTimerService::previousPreset()
{
    answer(m_commands.stepPreset(-1));
}

bool DBusTimerService::startPreset(const QString &nameOrUuid)
{
    return answer(m_commands.startPreset(nameOrUuid));
}

void DBusTimerService::toggle()
{
    answer(m_commands.toggle());
}

void DBusTimerService::start()
{
    answer(m_commands.start());
}

void DBusTimerService::pause()
{
    answer(m_commands.pause());
}

void DBusTimerService::stop()
{
    answer(m_commands.stop());
}

void DBusTimerService::skip()
{
    answer(m_commands.skip());
}

void DBusTimerService::extend()
{
    answer(m_commands.extend());
}

void DBusTimerService::extendBy(int minutes)
{
    answer(m_commands.extend(minutes));
}

QString DBusTimerService::status() const
{
    return m_commands.status();
}

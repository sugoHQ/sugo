// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>

#include <functional>

#include "SyncChannel.h"
#include "TimerPreset.h"

class PresetModel;
class PresetPersistence;
class PresetRepository;

/**
 * The timers as a sync channel (PresetSyncFormat): the presets of PresetModel and the
 * tombstones of PresetRepository, stored through PresetPersistence.
 *
 * A merge that would delete the selected timer while `timerBusy` says a phase runs or is paused
 * is held back (switching timers would stop the phase). While saves fail, the model may already
 * hold a merge the database lacks: every merge is stored again then.
 */
class PresetSyncChannel : public QObject, public SyncChannel
{
    Q_OBJECT

public:
    /// Not owned; they must outlive this object.
    PresetSyncChannel(PresetModel *presets, PresetPersistence *persistence, PresetRepository *repository,
                      std::function<bool()> timerBusy, QObject *parent = nullptr);

    SyncKind kind() const override { return SyncKind::Presets; }
    bool ready() const override;
    QByteArray localData(const QString &deviceId, qsizetype maxBytes) override;
    Merge merge(const QByteArray &plaintext, const QString &deviceId) override;

private:
    /// False if the database did not store them.
    bool applyLocally(const QList<TimerPreset> &presets, const QList<PresetTombstone> &tombstones);
    bool wouldStopTimer(const QList<TimerPreset> &presets) const;

    QPointer<PresetModel> m_presets;
    QPointer<PresetPersistence> m_persistence;
    PresetRepository *m_repository = nullptr;
    std::function<bool()> m_timerBusy;
    bool m_applying = false;
};

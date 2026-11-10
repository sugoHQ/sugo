// SPDX-License-Identifier: GPL-3.0-or-later
#include "PresetSyncChannel.h"

#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "PresetSyncFormat.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(lcSyncController)

PresetSyncChannel::PresetSyncChannel(PresetModel *presets, PresetPersistence *persistence, PresetRepository *repository,
                                     std::function<bool()> timerBusy, QObject *parent)
    : QObject(parent)
    , m_presets(presets)
    , m_persistence(persistence)
    , m_repository(repository)
    , m_timerBusy(std::move(timerBusy))
{
    if (m_persistence) {
        connect(m_persistence, &PresetPersistence::saved, this, [this]() {
            if (!m_applying) {
                notifyChanged();
            }
        });
    }
}

bool PresetSyncChannel::ready() const
{
    // Timers that could not be read are neither saved nor synced: an upload would replace the
    // synced ones with the built-in timer alone.
    return m_presets && m_persistence && m_repository && !m_persistence->loadFailed();
}

QByteArray PresetSyncChannel::localData(const QString &deviceId, qsizetype maxBytes)
{
    Q_UNUSED(maxBytes) // a timer is never dropped to fit: over the limit, the upload is refused
    PresetSyncFormat::Data data;
    data.presets = m_presets->presets();
    data.tombstones = m_repository->tombstones();
    return PresetSyncFormat::toJson(data, deviceId);
}

SyncChannel::Merge PresetSyncChannel::merge(const QByteArray &plaintext, const QString &deviceId)
{
    Merge out;
    const PresetSyncFormat::ParseResult parsed = PresetSyncFormat::fromJson(plaintext);
    if (!parsed.ok) {
        out.result = Merge::Result::Unreadable;
        return out;
    }
    out.newerFormat = parsed.newerFormat();

    PresetSyncFormat::Data local;
    local.presets = m_presets->presets();
    local.tombstones = m_repository->tombstones();
    const PresetSyncFormat::MergeResult merged =
        PresetSyncFormat::merge(local, parsed.data, deviceId, QDateTime::currentMSecsSinceEpoch());
    if (merged.changedLocal && wouldStopTimer(merged.result.presets)) {
        qCInfo(lcSyncController) << "The selected timer was deleted on another device; waiting for the timer to stop";
        out.result = Merge::Result::HeldBack;
        return out;
    }
    // While saves fail, the model may already hold the merge the database lacks: store it again.
    if ((merged.changedLocal || m_persistence->isSaveFailing()) && !applyLocally(merged.result.presets, merged.result.tombstones)) {
        out.result = Merge::Result::LocalSaveFailed;
        return out;
    }
    out.changedRemote = merged.changedRemote;
    return out;
}

bool PresetSyncChannel::applyLocally(const QList<TimerPreset> &presets, const QList<PresetTombstone> &tombstones)
{
    // Neither the model nor the save may answer with an upload of what just arrived.
    m_applying = true;
    // A selected timer deleted elsewhere falls back to the built-in one.
    m_presets->setPresets(presets, m_presets->currentUuid());
    const bool stored = m_persistence->save(tombstones);
    m_applying = false;
    return stored;
}

bool PresetSyncChannel::wouldStopTimer(const QList<TimerPreset> &presets) const
{
    if (!m_timerBusy || !m_timerBusy()) {
        return false;
    }
    const QString current = m_presets->currentUuid();
    if (current == PresetModel::DefaultUuid) {
        return false; // always kept (PresetModel::setPresets())
    }
    return std::none_of(presets.cbegin(), presets.cend(), [&current](const TimerPreset &p) { return p.uuid == current; });
}

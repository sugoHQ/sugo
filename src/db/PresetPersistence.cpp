// SPDX-License-Identifier: GPL-3.0-or-later
#include "PresetPersistence.h"

#include "PresetModel.h"
#include "PresetRepository.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcPresetPersistence, "sugo.db.persistence")

PresetPersistence::PresetPersistence(PresetModel *model, PresetRepository *repository, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_repository(repository)
{
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(60 * 1000);
    connect(&m_retryTimer, &QTimer::timeout, this, &PresetPersistence::savePresets);

    bool ok = false;
    const QList<TimerPreset> stored = m_repository->loadAll(&ok);
    if (ok && !stored.isEmpty()) {
        model->setPresets(stored, m_repository->currentUuid());
    } else if (ok) {
        // First run: store the built-in preset so the database is never empty afterwards.
        savePresets();
        saveSelection();
    } else {
        // Saving now would replace the stored presets (which we could not read) with the
        // built-in one, deleting the user's timers. Run with the built-in preset, unsaved.
        qCWarning(lcPresetPersistence) << "Cannot load the stored timers; changes will not be saved this session";
        m_loadFailed = true;
        return;
    }

    // Connect only after loading, so loading never writes back.
    connect(model, &PresetModel::modified, this, &PresetPersistence::savePresets);
    connect(model, &PresetModel::currentChanged, this, &PresetPersistence::saveSelection);
}

void PresetPersistence::savePresets()
{
    save();
}

bool PresetPersistence::save(const QList<PresetTombstone> &received)
{
    if (m_loadFailed) {
        // The model does not hold the stored timers: saveAll() would delete them (and stamp
        // each as deleted here, for the other computers too).
        return false;
    }
    // Deletions received with a save that failed are kept, a newer one for the same timer replacing it;
    // saveAll() drops those of timers the model has again.
    QList<PresetTombstone> tombstones = m_pendingReceived;
    for (const PresetTombstone &t : received) {
        tombstones.removeIf([&t](const PresetTombstone &pending) { return pending.uuid == t.uuid; });
        tombstones.append(t);
    }
    const bool stored = m_repository->saveAll(m_model->presets(), tombstones);
    if (stored) {
        m_pendingReceived.clear();
    } else {
        // Without them the retry would stamp each of these timers as deleted here, now.
        m_pendingReceived = tombstones;
    }
    setSaveFailing(!stored);
    if (stored) {
        saveSelection();
        Q_EMIT saved();
    }
    return stored;
}

void PresetPersistence::saveSelection()
{
    if (m_saveFailing) {
        // The retry saves everything, the selection included.
        return;
    }
    setSaveFailing(!m_repository->setCurrentUuid(m_model->currentUuid()));
}

void PresetPersistence::setSaveFailing(bool failing)
{
    if (failing) {
        qCWarning(lcPresetPersistence) << "Cannot save the timers; trying again later";
        m_retryTimer.start();
    } else {
        m_retryTimer.stop();
    }
    if (failing != m_saveFailing) {
        m_saveFailing = failing;
        Q_EMIT saveFailingChanged();
    }
}

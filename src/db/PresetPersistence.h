// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QTimer>

#include "TimerPreset.h"

class PresetModel;
class PresetRepository;

/**
 * Loads the presets into the model at construction and saves every later change.
 *
 * Saving is synchronous: a handful of rows in a WAL database costs well under a
 * millisecond, and it means a crash can lose at most the edit in progress. A save the database
 * refuses is tried again later (and at the next change); isSaveFailing() says so meanwhile.
 * Every stored change of the timers is announced by saved() (Sugo Sync uploads them).
 */
class PresetPersistence : public QObject
{
    Q_OBJECT

public:
    PresetPersistence(PresetModel *model, PresetRepository *repository, QObject *parent = nullptr);

    /// True while the latest changes could not be stored.
    bool isSaveFailing() const { return m_saveFailing; }
    /// True when the stored timers could not be read at startup: nothing is saved this session
    /// (the model holds only the built-in timer, and saving it would delete the stored ones).
    bool loadFailed() const { return m_loadFailed; }
    void setRetryIntervalForTesting(int ms) { m_retryTimer.setInterval(ms); }

    /**
     * Stores the model's timers now, with the selection: what a merge from another computer
     * calls after PresetModel::setPresets(), which does not announce itself. `received` are
     * deletions made on other computers (PresetRepository::saveAll()). False if the database
     * refused; then it is tried again later, as for any change, and the retry (or the next
     * save) still stores `received` as they came, until one succeeds.
     */
    bool save(const QList<PresetTombstone> &received = {});

Q_SIGNALS:
    void saveFailingChanged();
    /// The timers were stored (not for a change of the selection alone).
    void saved();

private:
    void savePresets();
    void saveSelection();
    void setSaveFailing(bool failing);

    PresetModel *m_model;
    PresetRepository *m_repository;
    bool m_saveFailing = false;
    bool m_loadFailed = false;
    /// Deletions received with a save that failed, kept until a save stores them.
    QList<PresetTombstone> m_pendingReceived;
    QTimer m_retryTimer;
};

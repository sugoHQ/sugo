// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

#include "SyncKind.h"

/**
 * One kind of synced data for SyncController: how its local data becomes the plaintext of its
 * blob, and how a downloaded plaintext merges into it. The controller does the rest for every
 * channel alike: transfers, ETag, "to upload" flag, conflicts, retries, encryption, size limit
 * and the kinds that need Plus.
 */
class SyncChannel
{
public:
    struct Merge {
        enum class Result {
            Merged,          ///< Merged, and stored here if it changed anything.
            Unreadable,      ///< Not this kind's format: never overwritten until it opens.
            HeldBack,        ///< Not applied now (it would disturb the user): downloaded again on resume.
            LocalSaveFailed, ///< The merge could not be stored here: downloaded again later.
        };
        Result result = Result::Merged;
        bool newerFormat = false;   ///< Written by a newer Sugo: read, never written here.
        bool changedRemote = false; ///< The server lacks something of this device: upload.
    };

    virtual ~SyncChannel() = default;

    virtual SyncKind kind() const = 0;
    /// Whether its data is at hand (the controller syncs only ready channels).
    virtual bool ready() const = 0;
    /**
     * The plaintext to upload (versioned JSON), written by `deviceId`. A channel that can drop
     * old data (compaction) keeps it within `maxBytes`; the controller refuses a larger blob.
     * Empty when it cannot be written.
     */
    virtual QByteArray localData(const QString &deviceId, qsizetype maxBytes) = 0;
    /// Merges what the server holds into the local data, as `deviceId`.
    virtual Merge merge(const QByteArray &plaintext, const QString &deviceId) = 0;

    /// The controller's callback for a change made here (never called for what merge() applied).
    void setChangedHandler(std::function<void()> changed) { m_changed = std::move(changed); }

protected:
    void notifyChanged() const
    {
        if (m_changed) {
            m_changed();
        }
    }

private:
    std::function<void()> m_changed;
};

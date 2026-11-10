// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLatin1String>
#include <QtGlobal>

/// A kind of synced data: each has its own encrypted blob, ETag and life cycle (docs/api.md of
/// the sugo-sync repository, "Blob").
enum class SyncKind {
    Presets,
    Sessions,
    Tasks,
    Statistics,
    Settings,
};

namespace SyncKinds
{
/// What a blob adds to its plaintext: "SUGO", the format byte, the nonce and the tag.
constexpr qsizetype BlobOverhead = 4 + 1 + 24 + 16;

/// The kind's name in /v1/blob/{kind} and in the blob's associated data.
inline QLatin1String name(SyncKind kind)
{
    switch (kind) {
    case SyncKind::Presets:
        return QLatin1String("presets");
    case SyncKind::Sessions:
        return QLatin1String("sessions");
    case SyncKind::Tasks:
        return QLatin1String("tasks");
    case SyncKind::Statistics:
        return QLatin1String("statistics");
    case SyncKind::Settings:
        return QLatin1String("settings");
    }
    return QLatin1String("presets");
}

/// The largest blob the server takes for the kind, in bytes.
constexpr qsizetype maxBytes(SyncKind kind)
{
    switch (kind) {
    case SyncKind::Presets:
        return 256 * 1024;
    case SyncKind::Sessions:
    case SyncKind::Tasks:
    case SyncKind::Statistics:
        return 1024 * 1024;
    case SyncKind::Settings:
        return 8 * 1024;
    }
    return 0;
}

/// Whether the kind syncs only with Plus (the server answers 403 plus_required otherwise).
constexpr bool needsPlus(SyncKind kind)
{
    return kind != SyncKind::Presets;
}
} // namespace SyncKinds

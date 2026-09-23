// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "FolderAccessGuard.h"

#include <QSettings>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <utility>

namespace {

constexpr auto kSettingsKey = "allowedFolders";

// Canonicalizes the nearest ancestor that exists, for not-yet-created paths.
QString resolveExistingAncestor(const QString &path)
{
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty() && !QFileInfo::exists(current)) {
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
    const QString canonical = QFileInfo(current).canonicalFilePath();
    return canonical.isEmpty() ? current : canonical;
}

QString canonicalRoot(const QString &root)
{
    const QString canonical = QFileInfo(root).canonicalFilePath();
    return canonical.isEmpty() ? QFileInfo(root).absoluteFilePath() : canonical;
}

} // namespace

namespace FolderAccessGuard {

QString appDataFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QStringList allowedFolders()
{
    QSettings settings;
    return settings.value(QLatin1String(kSettingsKey)).toStringList();
}

void addAllowedFolder(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString canonical = canonicalRoot(path);

    QSettings settings;
    QStringList folders = settings.value(QLatin1String(kSettingsKey)).toStringList();
    if (!folders.contains(canonical))
        folders.append(canonical);
    settings.setValue(QLatin1String(kSettingsKey), folders);
}

void removeAllowedFolder(const QString &path)
{
    const QString canonical = canonicalRoot(path);

    QSettings settings;
    QStringList folders = settings.value(QLatin1String(kSettingsKey)).toStringList();
    folders.removeAll(canonical);
    folders.removeAll(path);
    settings.setValue(QLatin1String(kSettingsKey), folders);
}

bool isAllowed(const QString &path)
{
    if (path.isEmpty())
        return false;

    const QString target = resolveExistingAncestor(path);
    if (target.isEmpty())
        return false;

    QStringList roots = allowedFolders();
    roots.append(appDataFolder());

    for (const QString &root : std::as_const(roots)) {
        const QString rootPath = canonicalRoot(root);
        if (rootPath.isEmpty())
            continue;
        if (target == rootPath || target.startsWith(rootPath + QLatin1Char('/')))
            return true;
    }
    return false;
}

} // namespace FolderAccessGuard

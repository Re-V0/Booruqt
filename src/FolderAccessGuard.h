// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QStringList>

// App-level discipline, not an OS sandbox: gates file writes/deletes.
namespace FolderAccessGuard {

QString appDataFolder();

QStringList allowedFolders();
void addAllowedFolder(const QString &path);
void removeAllowedFolder(const QString &path);

// True if `path` resolves inside the app data folder or an allowed one.
bool isAllowed(const QString &path);

} // namespace FolderAccessGuard

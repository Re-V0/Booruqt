// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QStringList>

class LibraryDatabase;

// Puts pictures and videos from anywhere on the computer into the library:
// a copy in the chosen folder, indexed like a download.
namespace LibraryImport {

// The source name imported files get in the library, as a site's name is
// for a download.
inline const QString kSourceName = QStringLiteral("Local");

struct Result {
    int imported = 0;
    // The same file (by content) is in the library already.
    int alreadyInLibrary = 0;
    // Not a picture or video the library can show.
    int notMedia = 0;
    int failed = 0;
    // One line per failure: which file and why.
    QStringList errors;
    // The library's copy of each imported file.
    QStringList importedPaths;
};

// Whether a file is a picture (anything Qt reads) or a video, by its name.
bool isMedia(const QString &path);

// Imports files and the media inside folders (with their subfolders) into
// targetDir. The originals stay where they are. A file already inside
// libraryRoot is indexed where it lies instead of being copied. Every
// imported file gets `tags`, together with those of a tags sidecar it
// already has; with any tags, the copy gets a sidecar too.
Result importPaths(LibraryDatabase *library, const QStringList &paths, const QString &targetDir,
                   const QString &libraryRoot, const QStringList &tags = {});

} // namespace LibraryImport

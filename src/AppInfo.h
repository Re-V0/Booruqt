// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QStringList>
#include <QFile>
#include <QCoreApplication>

#ifndef BLAIRQT_VERSION
#define BLAIRQT_VERSION "0.0.0-dev"
#endif

// One User-Agent for every request. Sites like e621 require one that names
// the client, and it has to track the real version, not a frozen literal.
inline QString blairqtUserAgent()
{
    return QStringLiteral("blairqt/" BLAIRQT_VERSION " (Qt6 booru client)");
}

// Finds a file shipped with the program (translations/, plugin_runner.py,
// tools/). Looked for in this order:
//   1. next to the executable: a development build, or the Windows zip
//   2. ../Resources from the executable: inside a macOS .app bundle
//   3. the install data dir: Linux and FreeBSD packages (share/blairqt)
// Returns the first candidate that exists. If none does, returns the last
// one, so an error message can still say where the file was expected.
inline QString findAppFile(const QString &relativePath)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates.append(appDir + QLatin1Char('/') + relativePath);
    candidates.append(appDir + QStringLiteral("/../Resources/") + relativePath);
#ifdef BLAIRQT_DATA_DIR
    candidates.append(QStringLiteral(BLAIRQT_DATA_DIR "/") + relativePath);
#endif
    for (const QString &candidate : candidates) {
        if (QFile::exists(candidate))
            return candidate;
    }
    return candidates.last();
}

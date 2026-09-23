// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QObject>
#include <QString>
#include "Post.h"

class QNetworkAccessManager;
class QNetworkReply;
class LibraryDatabase;

// Downloads post images, indexes them, writes a hidden tags sidecar.
class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(LibraryDatabase *library, QObject *parent = nullptr);

    void setDownloadDir(const QString &dir);
    QString downloadDir() const { return m_downloadDir; }

    void download(const Post &post);

    // So callers deleting an image can find its sidecar too.
    static QString sidecarPathFor(const QString &imagePath);

signals:
    // path is empty on failure; skipped means it already existed.
    void downloadFinished(QString path, bool ok, bool skipped, QString error);

private:
    QString targetFilePath(const Post &post) const;

    QNetworkAccessManager *m_manager;
    LibraryDatabase *m_library;
    QString m_downloadDir;
};

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QSet>
#include "Post.h"

class QNetworkAccessManager;
class QNetworkReply;
class LibraryDatabase;

// Downloads post images, indexes them, writes a hidden tags sidecar.
//
// At most kMaxParallel downloads run at once; the rest wait here, not inside
// Qt. Qt would also hold back whatever exceeds its six connections per host,
// but a request's timeout already runs while it waits there, so the tail of
// a large batch used to time out without ever having been sent.
class DownloadManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxParallel = 6;

    explicit DownloadManager(LibraryDatabase *library, QObject *parent = nullptr);

    void setDownloadDir(const QString &dir);
    QString downloadDir() const { return m_downloadDir; }

    void download(const Post &post);

    // A download is given up after this long without receiving anything.
    // 30 s by default.
    void setTransferTimeout(int milliseconds) { m_transferTimeoutMs = milliseconds; }

    // Downloads running right now, and those waiting for a free slot.
    int runningCount() const { return m_running; }
    int queuedCount() const { return int(m_queue.size()); }

    // So callers deleting an image can find its sidecar too.
    static QString sidecarPathFor(const QString &imagePath);

signals:
    // path is empty on failure; skipped means it already existed.
    void downloadFinished(QString path, bool ok, bool skipped, QString error);

private:
    struct Queued {
        Post post;
        QString path;
    };

    QString targetFilePath(const Post &post) const;
    void startQueued();
    void start(const Post &post, const QString &path);

    QNetworkAccessManager *m_manager;
    LibraryDatabase *m_library;
    QString m_downloadDir;
    // Target paths of downloads running or waiting.
    QSet<QString> m_inFlight;
    QList<Queued> m_queue;
    int m_running = 0;
    int m_transferTimeoutMs = 30000;
};

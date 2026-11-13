// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QObject>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QUrl>
#include <QHash>
#include <QSize>

class QIODevice;
class QNetworkAccessManager;
class QNetworkReply;
class VideoStill;

// Fetches and decodes preview images off the GUI thread, with disk caching.
//
// A remote preview is downloaded. A local file (the library's posts) is read
// straight from disk by the image decoder, never as a whole into memory; a
// local video gives its first frame.
class ThumbnailLoader : public QObject {
    Q_OBJECT
public:
    // Videos opened at once for their first frame: each one starts a decoder.
    static constexpr int kMaxVideoStills = 2;

    // useDiskCache: keep remote thumbnails on disk. Off for local files,
    // which are already on disk.
    explicit ThumbnailLoader(QObject *parent = nullptr, bool useDiskCache = true);

    void setTargetSize(const QSize &size) { m_targetSize = size; }
    QSize targetSize() const { return m_targetSize; }

    void request(int row, const QUrl &url);
    void cancelAll();

    // Decodes an image at the size it will be shown at: it fits inside
    // target and keeps its proportions. The decoder scales while it reads
    // (a JPEG at a fraction of its size), which is faster than decoding it
    // whole and takes a fraction of the memory. Null if it is no image.
    static QImage decodeScaled(QIODevice *device, const QSize &target);

signals:
    void thumbnailReady(int row, QPixmap pixmap);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    struct VideoJob {
        int row = 0;
        QString path;
    };

    void requestLocal(int row, const QString &path);
    void startQueuedVideoStills();

    QNetworkAccessManager *m_manager;
    QSize m_targetSize{220, 220};
    QHash<QNetworkReply *, int> m_pendingRows;
    // Bumped by cancelAll(); a decode started under an older value is dropped.
    int m_generation = 0;
    // Local videos waiting for their first frame, and the ones being read.
    QList<VideoJob> m_videoQueue;
    QList<VideoStill *> m_videoStills;
};

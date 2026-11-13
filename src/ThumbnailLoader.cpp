// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ThumbnailLoader.h"
#include "SiteAccess.h"
#include "AppInfo.h"
#include "Post.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtConcurrent/QtConcurrent>
#include <functional>
#include <utility>

namespace {

// A video that shows no frame within this time gets no thumbnail.
constexpr int kVideoStillTimeoutMs = 8000;

} // namespace

// Opens a local video and hands over its first frame, or a null image when
// it cannot be played. Lives until that answer; cancel() silences it.
class VideoStill : public QObject {
public:
    using Handler = std::function<void(VideoStill *, const QImage &)>;

    VideoStill(const QString &path, Handler done, QObject *parent)
        : QObject(parent), m_done(std::move(done))
    {
        // No audio output is set, so nothing is ever heard.
        m_player.setVideoSink(&m_sink);
        connect(&m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
            const QImage image = frame.toImage();
            if (!image.isNull())
                finish(image);
        });
        connect(&m_player, &QMediaPlayer::errorOccurred, this, [this]() { finish({}); });
        connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::EndOfMedia)
                finish({});
        });
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this]() { finish({}); });
        m_timeout.start(kVideoStillTimeoutMs);

        m_player.setSource(QUrl::fromLocalFile(path));
        m_player.play();
    }

    void cancel()
    {
        m_done = nullptr;
        finish({});
    }

private:
    void finish(const QImage &image)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_timeout.stop();
        m_player.stop();
        if (m_done)
            m_done(this, image);
    }

    Handler m_done;
    bool m_finished = false;
    QTimer m_timeout;
    // Destroyed in reverse order: the player before its sink.
    QVideoSink m_sink;
    QMediaPlayer m_player;
};

ThumbnailLoader::ThumbnailLoader(QObject *parent, bool useDiskCache)
    : QObject(parent), m_manager(new QNetworkAccessManager(this))
{
    // One cache directory must have one QNetworkDiskCache, or two instances
    // expire and overwrite each other's entries.
    if (useDiskCache) {
        auto *cache = new QNetworkDiskCache(this);
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/blairqt/thumbnails");
        cache->setCacheDirectory(cacheDir);
        m_manager->setCache(cache);
    }

    connect(m_manager, &QNetworkAccessManager::finished, this, &ThumbnailLoader::onReplyFinished);
}

QImage ThumbnailLoader::decodeScaled(QIODevice *device, const QSize &target)
{
    QImageReader reader(device);
    const QSize full = reader.size();
    // Some formats cannot tell their size before decoding; those are read
    // whole and scaled afterwards.
    if (full.isValid() && !full.isEmpty())
        reader.setScaledSize(full.scaled(target, Qt::KeepAspectRatio));

    const QImage image = reader.read();
    if (image.isNull())
        return {};
    const QSize wanted = image.size().scaled(target, Qt::KeepAspectRatio);
    if (image.size() == wanted)
        return image;
    return image.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void ThumbnailLoader::request(int row, const QUrl &url)
{
    if (!url.isValid())
        return;

    if (url.isLocalFile()) {
        requestLocal(row, url.toLocalFile());
        return;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    req.setTransferTimeout(15000);

    QNetworkReply *reply = SiteAccess::get(m_manager, req);
    m_pendingRows.insert(reply, row);
}

void ThumbnailLoader::requestLocal(int row, const QString &path)
{
    // A video is no image: the network stack would read all of it into
    // memory (hundreds of megabytes) only for the decoder to reject it.
    if (isVideoExtension(QFileInfo(path).suffix())) {
        m_videoQueue.append({row, path});
        startQueuedVideoStills();
        return;
    }

    const QSize targetSize = m_targetSize;
    const int generation = m_generation;

    // Decode off-thread as QImage; QPixmap is GUI-thread only.
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, row, generation]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return; // cancelAll() ran since: this row number now means another post
        if (!image.isNull())
            emit thumbnailReady(row, QPixmap::fromImage(image));
    });
    watcher->setFuture(QtConcurrent::run([path, targetSize]() -> QImage {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return decodeScaled(&file, targetSize);
    }));
}

void ThumbnailLoader::startQueuedVideoStills()
{
    while (m_videoStills.size() < kMaxVideoStills && !m_videoQueue.isEmpty()) {
        const VideoJob job = m_videoQueue.takeFirst();
        const int row = job.row;
        auto *still = new VideoStill(job.path, [this, row](VideoStill *finished, const QImage &frame) {
            if (!frame.isNull()) {
                const QSize wanted = frame.size().scaled(m_targetSize, Qt::KeepAspectRatio);
                emit thumbnailReady(row, QPixmap::fromImage(
                    frame.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
            }
            // This runs inside one of the player's own signal handlers, so
            // the player is deleted from the event loop instead. Only then
            // does the next video start: a player holds tens of megabytes of
            // read-ahead, and they must not pile up.
            const QPointer<VideoStill> guard(finished);
            QMetaObject::invokeMethod(this, [this, guard]() {
                if (guard) {
                    m_videoStills.removeOne(guard.data());
                    delete guard.data();
                }
                startQueuedVideoStills();
            }, Qt::QueuedConnection);
        }, this);
        m_videoStills.append(still);
    }
}

void ThumbnailLoader::cancelAll()
{
    // abort() emits finished() synchronously and onReplyFinished() would
    // take() from m_pendingRows mid-iteration: copy, clear, then abort.
    const QList<QNetworkReply *> replies = m_pendingRows.keys();
    m_pendingRows.clear();
    for (QNetworkReply *reply : replies)
        reply->abort();

    // Decodes already running off-thread still carry the old row numbers.
    ++m_generation;

    m_videoQueue.clear();
    const QList<VideoStill *> stills = m_videoStills;
    m_videoStills.clear();
    for (VideoStill *still : stills) {
        still->cancel();
        still->deleteLater();
    }
}

void ThumbnailLoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    // Unknown reply (cancelAll() already dropped it): no row to attribute it to.
    if (!m_pendingRows.contains(reply))
        return;
    const int row = m_pendingRows.take(reply);
    if (reply->error() != QNetworkReply::NoError)
        return;

    const QByteArray bytes = reply->readAll();
    const QSize targetSize = m_targetSize;
    const int generation = m_generation;

    // Decode off-thread as QImage; QPixmap is GUI-thread only.
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, row, generation]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return; // cancelAll() ran since: this row number now means another post
        if (!image.isNull())
            emit thumbnailReady(row, QPixmap::fromImage(image));
    });

    watcher->setFuture(QtConcurrent::run([bytes, targetSize]() -> QImage {
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly))
            return {};
        return decodeScaled(&buffer, targetSize);
    }));
}

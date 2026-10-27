// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "AnimatedThumbnails.h"
#include "AppInfo.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "Post.h"
#include "SiteAccess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QListView>
#include <QMediaPlayer>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryFile>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <functional>
#include <memory>
#include <utility>

namespace {

const char kSettingsKey[] = "animatedThumbnails";

// About 15 frames per second: enough to see the motion, and each frame of
// a video costs a conversion to QImage.
constexpr int kFrameIntervalMs = 66;

} // namespace

// Plays one row's media and hands its frames over. Lives only while the
// row plays; stop() silences it at once, before it is deleted.
class ThumbnailAnimation : public QObject {
public:
    using FrameHandler = std::function<void(const QImage &)>;
    using FailHandler = std::function<void()>;

    ThumbnailAnimation(const MediaLink &media, QNetworkAccessManager *network,
                       FrameHandler onFrame, FailHandler onFailed, QObject *parent)
        : QObject(parent), m_video(media.video), m_onFrame(std::move(onFrame)), m_onFailed(std::move(onFailed))
    {
        const QUrl url(media.url);
        if (url.isLocalFile())
            play(url.toLocalFile());
        else
            download(url, network);
    }

    ~ThumbnailAnimation() override
    {
        stop();
    }

    void stop()
    {
        m_stopped = true;
        if (m_reply) {
            disconnect(m_reply, nullptr, this, nullptr);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        if (m_player) {
            m_player->stop();
            m_player->setSource(QUrl());
        }
        if (m_movie)
            m_movie->stop();
    }

private:
    void download(const QUrl &url, QNetworkAccessManager *network)
    {
        QString suffix = QFileInfo(url.path()).suffix().toLower();
        if (!isVideoExtension(suffix) && suffix != QStringLiteral("gif"))
            suffix.clear();
        QString name = QDir::tempPath() + QStringLiteral("/blairqt-XXXXXX");
        if (!suffix.isEmpty())
            name += QLatin1Char('.') + suffix;
        m_file = std::make_unique<QTemporaryFile>(name);
        if (!m_file->open()) {
            fail();
            return;
        }

        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
        req.setTransferTimeout(30000);
        SiteAccess::apply(req);
        m_reply = network->get(req);

        connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
            if (total > AnimatedThumbnails::kMaxDownloadBytes || received > AnimatedThumbnails::kMaxDownloadBytes)
                m_reply->abort();
        });
        connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
            const QByteArray chunk = m_reply->readAll();
            if (m_file->write(chunk) != chunk.size())
                m_reply->abort();
        });
        connect(m_reply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                fail();
                return;
            }
            m_file->write(reply->readAll());
            m_file->close();
            play(m_file->fileName());
        });
    }

    void play(const QString &path)
    {
        if (m_video) {
            m_sink = std::make_unique<QVideoSink>();
            m_player = std::make_unique<QMediaPlayer>();
            // No audio output is set, so nothing is ever heard.
            m_player->setVideoSink(m_sink.get());
            m_player->setLoops(QMediaPlayer::Infinite);
            connect(m_player.get(), &QMediaPlayer::errorOccurred, this, [this]() { fail(); });
            // Some backends ignore setLoops() (FFmpeg in Qt 6.4): start over by hand.
            connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia && !m_stopped) {
                    m_player->setPosition(0);
                    m_player->play();
                }
            });
            connect(m_sink.get(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
                if (frameDue())
                    deliver(frame.toImage());
            });
            m_player->setSource(QUrl::fromLocalFile(path));
            m_player->play();
            return;
        }

        m_movie = std::make_unique<QMovie>(path);
        if (!m_movie->isValid()) {
            fail();
            return;
        }
        connect(m_movie.get(), &QMovie::frameChanged, this, [this]() {
            if (frameDue())
                deliver(m_movie->currentImage());
        });
        m_movie->start();
    }

    bool frameDue()
    {
        if (m_stopped)
            return false;
        if (m_sinceFrame.isValid() && m_sinceFrame.elapsed() < kFrameIntervalMs)
            return false;
        m_sinceFrame.start();
        return true;
    }

    void deliver(const QImage &image)
    {
        if (!m_stopped && !image.isNull())
            m_onFrame(image);
    }

    void fail()
    {
        if (m_stopped)
            return;
        stop();
        m_onFailed();
    }

    const bool m_video;
    const FrameHandler m_onFrame;
    const FailHandler m_onFailed;
    bool m_stopped = false;
    QNetworkReply *m_reply = nullptr;
    QElapsedTimer m_sinceFrame;
    // Destroyed in reverse order: the decoders before the file they read,
    // and the player before its sink.
    std::unique_ptr<QTemporaryFile> m_file;
    std::unique_ptr<QVideoSink> m_sink;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QMovie> m_movie;
};

AnimatedThumbnails::AnimatedThumbnails(QListView *view, GalleryModel *model, QObject *parent)
    : QObject(parent),
      m_view(view),
      m_model(model),
      m_network(new QNetworkAccessManager(this)),
      m_settleTimer(new QTimer(this))
{
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(300);
    connect(m_settleTimer, &QTimer::timeout, this, &AnimatedThumbnails::applyVisibleRows);

    // Players are keyed by row: anything that renumbers rows stops them all.
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() { stopAll(); m_failed.clear(); });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this]() { stopAll(); m_failed.clear(); });
    connect(m_model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int first, int) { onRowsInserted(first); });

    m_view->installEventFilter(this);
}

AnimatedThumbnails::~AnimatedThumbnails()
{
    stopAll();
}

bool AnimatedThumbnails::enabledInSettings()
{
    const QSettings settings;
    return settings.value(QLatin1String(kSettingsKey), false).toBool();
}

void AnimatedThumbnails::saveEnabledInSettings(bool enabled)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), enabled);
}

void AnimatedThumbnails::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (enabled) {
        refresh();
        return;
    }
    m_settleTimer->stop();
    stopAll();
    m_failed.clear();
}

void AnimatedThumbnails::refresh()
{
    if (m_enabled)
        m_settleTimer->start();
}

void AnimatedThumbnails::setSettleDelay(int milliseconds)
{
    m_settleTimer->setInterval(milliseconds);
}

bool AnimatedThumbnails::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view) {
        if (event->type() == QEvent::Hide)
            stopAll();
        else if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
            refresh();
    }
    return QObject::eventFilter(watched, event);
}

void AnimatedThumbnails::applyVisibleRows()
{
    if (!m_enabled)
        return;
    if (!m_view->isVisible()) {
        stopAll();
        return;
    }

    QList<int> wanted;
    const QVector<int> visible = GalleryView::visibleRows(m_view, m_model);
    for (const int row : visible) {
        if (wanted.size() == kMaxPlaying)
            break;
        if (m_failed.contains(row))
            continue;
        if (animatedThumbnailMedia(m_model->postAt(row)).url.isEmpty())
            continue;
        wanted.append(row);
    }

    const QList<int> playing = m_playing.keys();
    for (const int row : playing) {
        if (!wanted.contains(row))
            stopRow(row);
    }
    for (const int row : std::as_const(wanted)) {
        if (!m_playing.contains(row))
            startRow(row);
    }
}

void AnimatedThumbnails::startRow(int row)
{
    const MediaLink media = animatedThumbnailMedia(m_model->postAt(row));
    auto onFrame = [this, row](const QImage &image) {
        const QSize size = GalleryView::thumbnailSizeForCell(m_view->gridSize());
        m_model->setThumbnail(row, QPixmap::fromImage(image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    };
    auto onFailed = [this, row]() {
        m_failed.insert(row);
        stopRow(row);
        // Its place goes to the next animated row in view.
        refresh();
    };
    m_playing.insert(row, new ThumbnailAnimation(media, m_network, onFrame, onFailed, this));
}

void AnimatedThumbnails::stopRow(int row)
{
    ThumbnailAnimation *animation = m_playing.take(row);
    if (!animation)
        return;
    // Silenced now; deleted later, since this may run inside one of its own
    // signal handlers.
    animation->stop();
    animation->deleteLater();
}

void AnimatedThumbnails::stopAll()
{
    const QList<int> playing = m_playing.keys();
    for (const int row : playing)
        stopRow(row);
}

void AnimatedThumbnails::onRowsInserted(int first)
{
    // Appending (the usual case: the next page) leaves existing rows alone.
    for (auto it = m_playing.cbegin(); it != m_playing.cend(); ++it) {
        if (it.key() >= first) {
            stopAll();
            m_failed.clear();
            return;
        }
    }
}

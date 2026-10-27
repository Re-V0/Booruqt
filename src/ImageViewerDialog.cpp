// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ImageViewerDialog.h"
#include "SiteAccess.h"
#include "AppInfo.h"
#include "Post.h"

#include <QLabel>
#include <QScrollArea>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QScreen>
#include <QGuiApplication>
#include <QImageReader>
#include <QMovie>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>

namespace {

QNetworkRequest mediaRequest(const QString &url)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    // Aborts after 30 s without data, not after 30 s in total.
    req.setTransferTimeout(30000);
    SiteAccess::apply(req);
    return req;
}

} // namespace

ImageViewerDialog::ImageViewerDialog(const QString &url, bool video, const QString &title, QWidget *parent)
    : QDialog(parent), m_network(new QNetworkAccessManager(this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setText(tr("Wczytywanie..."));

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_imageLabel);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignCenter);

    m_fitCheck = new QCheckBox(tr("Dopasuj do okna"), this);
    m_fitCheck->setChecked(true);
    connect(m_fitCheck, &QCheckBox::toggled, this, &ImageViewerDialog::onFitToggled);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_scrollArea, 1);
    layout->addWidget(m_fitCheck);

    resize(800, 600);

    if (video) {
        // A video always fits the window, which opens at the video's size.
        m_fitCheck->hide();
        m_videoWidget = new QVideoWidget(this);
        m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
        m_videoWidget->hide();
        layout->insertWidget(1, m_videoWidget, 1);
        loadVideo(url);
    } else {
        loadImage(url);
    }
}

ImageViewerDialog::~ImageViewerDialog()
{
    if (m_videoReply) {
        disconnect(m_videoReply, nullptr, this, nullptr);
        m_videoReply->abort();
    }
    // Members go before children: stop the player while its file still
    // exists. On Windows an open file cannot be deleted.
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
}

void ImageViewerDialog::loadImage(const QString &url)
{
    QNetworkReply *reply = m_network->get(mediaRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_imageLabel->setText(tr("Błąd wczytywania: %1").arg(reply->errorString()));
            return;
        }
        showData(reply->readAll());
    });
}

void ImageViewerDialog::showData(const QByteArray &data)
{
    m_data = data;
    if (startAnimation())
        return;

    if (!m_fullPixmap.loadFromData(m_data)) {
        m_imageLabel->setText(tr("Nie udało się zdekodować obrazu."));
        return;
    }

    m_contentSize = m_fullPixmap.size();
    resizeToContent();
    updateDisplayedImage();
}

bool ImageViewerDialog::startAnimation()
{
    QBuffer probe(&m_data);
    probe.open(QIODevice::ReadOnly);
    QImageReader reader(&probe);
    if (!reader.supportsAnimation() || reader.imageCount() <= 1)
        return false;

    m_buffer.setBuffer(&m_data);
    m_buffer.open(QIODevice::ReadOnly);
    m_movie = std::make_unique<QMovie>(&m_buffer, QByteArray());
    if (!m_movie->isValid()) {
        m_movie.reset();
        m_buffer.close();
        return false; // fall back to showing the first frame as a still
    }

    m_movie->jumpToFrame(0);
    m_contentSize = m_movie->currentImage().size();
    m_imageLabel->setMovie(m_movie.get());
    m_movie->start();

    resizeToContent();
    updateDisplayedImage();
    return true;
}

void ImageViewerDialog::loadVideo(const QString &url)
{
    const QUrl parsed(url);
    if (parsed.isLocalFile()) {
        playVideo(parsed);
        return;
    }

    // The extension helps the decoder guess the container.
    QString suffix = QFileInfo(parsed.path()).suffix().toLower();
    if (!isVideoExtension(suffix))
        suffix.clear();
    QString name = QDir::tempPath() + QStringLiteral("/blairqt-XXXXXX");
    if (!suffix.isEmpty())
        name += QLatin1Char('.') + suffix;
    m_videoFile = std::make_unique<QTemporaryFile>(name);
    if (!m_videoFile->open()) {
        showMessage(tr("Nie udało się utworzyć pliku tymczasowego: %1").arg(m_videoFile->errorString()));
        return;
    }

    m_videoReply = m_network->get(mediaRequest(url));
    // Written as it arrives: a video can be large.
    connect(m_videoReply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_videoReply->readAll();
        if (m_videoFile->write(chunk) != chunk.size())
            m_videoReply->abort();
    });
    connect(m_videoReply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0)
            m_imageLabel->setText(tr("Wczytywanie... %1%").arg(received * 100 / total));
    });
    connect(m_videoReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_videoReply;
        m_videoReply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            showMessage(tr("Błąd wczytywania: %1").arg(reply->errorString()));
            return;
        }
        m_videoFile->write(reply->readAll());
        m_videoFile->close();
        playVideo(QUrl::fromLocalFile(m_videoFile->fileName()));
    });
}

void ImageViewerDialog::playVideo(const QUrl &file)
{
    m_player = new QMediaPlayer(this);
    // No audio output is set, so nothing is ever heard.
    m_player->setVideoOutput(m_videoWidget);
    m_player->setLoops(QMediaPlayer::Infinite);

    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
        showMessage(tr("Nie udało się odtworzyć wideo: %1").arg(text));
    });
    // Some backends ignore setLoops() (FFmpeg in Qt 6.4): start over by hand.
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            m_player->setPosition(0);
            m_player->play();
        }
    });
    // The first frame tells the size the window should open at.
    connect(m_videoWidget->videoSink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (!m_contentSize.isEmpty() || frame.size().isEmpty())
            return;
        m_contentSize = frame.size();
        resizeToContent();
    });

    m_scrollArea->hide();
    m_videoWidget->show();
    m_player->setSource(file);
    m_player->play();
}

void ImageViewerDialog::showMessage(const QString &text)
{
    if (m_videoWidget)
        m_videoWidget->hide();
    m_scrollArea->show();
    m_imageLabel->setText(text);
}

void ImageViewerDialog::resizeToContent()
{
    // Opens at the content's own size, capped to ~85% of the screen.
    if (m_contentSize.isEmpty())
        return;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize maxSize = screen->availableSize() * 0.85;
        resize(m_contentSize.boundedTo(maxSize).grownBy({40, 60, 40, 60}));
    }
}

void ImageViewerDialog::updateDisplayedImage()
{
    // A video scales itself (QVideoWidget, KeepAspectRatio).
    if (m_contentSize.isEmpty() || m_videoWidget)
        return;

    const QSize target = m_fitCheck->isChecked()
        ? m_contentSize.scaled(m_scrollArea->viewport()->size(), Qt::KeepAspectRatio)
        : m_contentSize;

    if (m_movie) {
        m_movie->setScaledSize(target);
        return;
    }

    m_imageLabel->setPixmap(target == m_contentSize
        ? m_fullPixmap
        : m_fullPixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ImageViewerDialog::onFitToggled(bool)
{
    updateDisplayedImage();
}

void ImageViewerDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_fitCheck && m_fitCheck->isChecked())
        updateDisplayedImage();
}

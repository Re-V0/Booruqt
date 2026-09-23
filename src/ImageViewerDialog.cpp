// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ImageViewerDialog.h"
#include "AppInfo.h"

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

ImageViewerDialog::ImageViewerDialog(const QString &imageUrl, const QString &title, QWidget *parent)
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

    QNetworkRequest req{QUrl(imageUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    req.setTransferTimeout(30000);

    QNetworkReply *reply = m_network->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_imageLabel->setText(tr("Błąd wczytywania: %1").arg(reply->errorString()));
            return;
        }
        showData(reply->readAll());
    });
}

ImageViewerDialog::~ImageViewerDialog() = default;

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
    if (m_contentSize.isEmpty())
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

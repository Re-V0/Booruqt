// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ThumbnailLoader.h"
#include "AppInfo.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QImage>

ThumbnailLoader::ThumbnailLoader(QObject *parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this))
{
    auto *cache = new QNetworkDiskCache(this);
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/blairqt/thumbnails");
    cache->setCacheDirectory(cacheDir);
    m_manager->setCache(cache);

    connect(m_manager, &QNetworkAccessManager::finished, this, &ThumbnailLoader::onReplyFinished);
}

void ThumbnailLoader::request(int row, const QUrl &url)
{
    if (!url.isValid())
        return;

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    req.setTransferTimeout(15000);

    QNetworkReply *reply = m_manager->get(req);
    m_pendingRows.insert(reply, row);
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
        QImage image;
        if (!image.loadFromData(bytes))
            return {};
        return image.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }));
}

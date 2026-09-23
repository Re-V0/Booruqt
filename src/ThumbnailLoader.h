// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QObject>
#include <QPixmap>
#include <QUrl>
#include <QHash>
#include <QSize>

class QNetworkAccessManager;
class QNetworkReply;

// Fetches and decodes preview images off the GUI thread, with disk caching.
class ThumbnailLoader : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailLoader(QObject *parent = nullptr);

    void setTargetSize(const QSize &size) { m_targetSize = size; }
    QSize targetSize() const { return m_targetSize; }

    void request(int row, const QUrl &url);
    void cancelAll();

signals:
    void thumbnailReady(int row, QPixmap pixmap);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_manager;
    QSize m_targetSize{220, 220};
    QHash<QNetworkReply *, int> m_pendingRows;
};

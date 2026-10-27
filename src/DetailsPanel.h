// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QWidget>
#include <QPixmap>
#include "Post.h"

class QLabel;
class QPushButton;

// Side panel: preview, tags, rating, and quick actions for a clicked post.
class DetailsPanel : public QWidget {
    Q_OBJECT
public:
    explicit DetailsPanel(QWidget *parent = nullptr);

    void showPost(const Post &post, const QPixmap &preview);
    void clear();

signals:
    // video: url plays as a video (see viewerMedia()).
    void zoomRequested(QString url, bool video, QString title);
    void downloadRequested(Post post);

private:
    QLabel *m_previewLabel;
    QLabel *m_infoLabel;
    QLabel *m_tagsLabel;
    QPushButton *m_openOriginalButton;
    QPushButton *m_openPageButton;
    QPushButton *m_zoomButton;
    QPushButton *m_downloadButton;

    Post m_currentPost;
    bool m_hasPost = false;
};

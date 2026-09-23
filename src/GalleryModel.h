// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QPixmap>
#include "Post.h"

class GalleryModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        ThumbnailRole = Qt::UserRole + 1,
        ThumbnailRequestedRole,
    };

    explicit GalleryModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void clear();
    void appendPosts(const QVector<Post> &posts);
    const Post &postAt(int row) const { return m_posts.at(row); }
    QPixmap thumbnailAt(int row) const;
    void setThumbnail(int row, const QPixmap &pixmap);
    void markThumbnailRequested(int row);
    // Sorts descending internally to avoid index-shifting bugs.
    void removeRows(const QList<int> &rows);

private:
    QVector<Post> m_posts;
    // View state lives here, not in Post: a Post stays plain data that is
    // safe to build off the GUI thread, unlike QPixmap. Invariant: all three
    // vectors always have the same size - only clear(), appendPosts() and
    // removeRows() may resize them, and each touches all three.
    QVector<QPixmap> m_thumbnails;
    QVector<bool> m_requested;
};

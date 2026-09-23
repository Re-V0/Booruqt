// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "GalleryModel.h"

#include <algorithm>

GalleryModel::GalleryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GalleryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_posts.size();
}

QVariant GalleryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_posts.size())
        return {};

    const Post &post = m_posts.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return post.tags.join(QStringLiteral(" "));
    case Qt::ToolTipRole:
        return post.tags.join(QStringLiteral(", "));
    // value(), not at(): a desync between the parallel vectors must not be
    // undefined behaviour in a paint path.
    case ThumbnailRole:
        return m_thumbnails.value(index.row());
    case ThumbnailRequestedRole:
        return m_requested.value(index.row());
    default:
        return {};
    }
}

QPixmap GalleryModel::thumbnailAt(int row) const
{
    if (row < 0 || row >= m_thumbnails.size())
        return {};
    return m_thumbnails.at(row);
}

void GalleryModel::clear()
{
    beginResetModel();
    m_posts.clear();
    m_thumbnails.clear();
    m_requested.clear();
    endResetModel();
}

void GalleryModel::appendPosts(const QVector<Post> &posts)
{
    if (posts.isEmpty())
        return;
    const int first = m_posts.size();
    const int last = first + posts.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    m_posts += posts;
    m_thumbnails.resize(m_posts.size());
    m_requested.resize(m_posts.size());
    endInsertRows();
}

void GalleryModel::setThumbnail(int row, const QPixmap &pixmap)
{
    if (row < 0 || row >= m_thumbnails.size())
        return;
    m_thumbnails[row] = pixmap;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ThumbnailRole});
}

void GalleryModel::markThumbnailRequested(int row)
{
    if (row < 0 || row >= m_requested.size())
        return;
    m_requested[row] = true;
}

void GalleryModel::removeRows(const QList<int> &rows)
{
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int row : sorted) {
        if (row < 0 || row >= m_posts.size())
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        m_posts.removeAt(row);
        m_thumbnails.removeAt(row);
        m_requested.removeAt(row);
        endRemoveRows();
    }
}

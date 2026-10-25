// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <QStringList>
#include "Post.h"

// SQLite-backed local library: downloaded posts, searchable by tag.
//
// Tags live twice: as the space-separated "tags" column of a post (what it
// was downloaded with, shown to the user) and one row per tag in post_tags
// (what search and suggestions use). Every write keeps the two in step.
class LibraryDatabase {
public:
    LibraryDatabase();
    ~LibraryDatabase();

    bool isOpen() const;

    // Updates the row if filePath already has an entry.
    void addOrUpdatePost(const Post &post, const QString &filePath);

    // Empty tagQuery returns everything; empty dirFilter means any folder.
    // Every tag in tagQuery must match a whole tag of the post (ignoring
    // ASCII case): "solo" does not find "solo_focus".
    // Paging is keyed, not offset-based: beforeId > 0 continues below that row
    // id, so deleting or moving files between pages cannot make rows repeat or
    // disappear the way a shifting OFFSET does.
    QVector<Post> search(const QString &tagQuery, int limit, qint64 beforeId = 0,
                         const QString &dirFilter = {}, bool includeSubdirs = false) const;

    // Path of an already-indexed post, or empty if unknown.
    QString pathForPost(const QString &source, const QString &postId) const;

    // Downloaded tags starting with prefix, most frequent first.
    QStringList suggestTags(const QString &prefix, int limit) const;

    int count() const;

    void removePost(const QString &filePath);

    // Leaves tags/rating/etc. untouched; only the path changes.
    void updatePostPath(const QString &oldPath, const QString &newPath);

private:
    void ensureSchema();
    // Schema 1 searched through an FTS5 index, which split tags at "_" and
    // "(". Schema 2 replaces it with the post_tags table.
    bool migrateToTagTable();
    // Rewrites post_tags for one post. Caller holds the transaction.
    bool replaceTags(qint64 rowId, const QString &tags);

    QSqlDatabase m_db;
};

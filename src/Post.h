// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

struct Post {
    QString id;
    QString sourceName;
    QStringList tags;
    QString rating;
    int score = 0;
    int width = 0;
    int height = 0;
    QString previewUrl;
    QString sampleUrl;
    QString fileUrl;
    QString postPageUrl;
    QString artistSourceUrl;

    // Set only for library posts: the downloaded file's path on disk and the
    // database row id, which paging uses as a stable cursor.
    QString localFilePath;
    qint64 libraryId = 0;
};

// The viewer decodes still images and animations, never video streams.
inline bool isVideoUrl(const QString &url)
{
    static const QStringList videoExtensions = {
        QStringLiteral(".webm"), QStringLiteral(".mp4"), QStringLiteral(".m4v"),
        QStringLiteral(".mov"), QStringLiteral(".mkv"), QStringLiteral(".avi"),
    };
    const QString path = QUrl(url).path().toLower();
    for (const QString &extension : videoExtensions) {
        if (path.endsWith(extension))
            return true;
    }
    return false;
}

// An already-downloaded file always wins: it opens instantly and works
// offline, while the stored remote URL may even be dead by now.
inline QString localImageUrl(const Post &post)
{
    return post.localFilePath.isEmpty() ? QString() : QUrl::fromLocalFile(post.localFilePath).toString();
}

// The real file when the site gives one: downloads, clipboard, "open original".
inline QString fullImageUrl(const Post &post)
{
    const QString local = localImageUrl(post);
    if (!local.isEmpty())
        return local;
    if (!post.fileUrl.isEmpty())
        return post.fileUrl;
    return post.sampleUrl.isEmpty() ? post.previewUrl : post.sampleUrl;
}

// For remote posts the viewer prefers the sample: originals can be tens of
// megabytes. A local file costs nothing, so it is used as-is. Video is
// skipped at every step - on a video post the thumbnail is the only frame
// the viewer can show, and an empty result means "nothing to zoom into".
inline QString viewerImageUrl(const Post &post)
{
    const QStringList candidates = {localImageUrl(post), post.sampleUrl, post.fileUrl, post.previewUrl};
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && !isVideoUrl(candidate))
            return candidate;
    }
    return {};
}

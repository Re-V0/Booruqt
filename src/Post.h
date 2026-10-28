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
    // The file's extension, for sites whose fileUrl has none of its own
    // (Shimmie2 CDNs serve "/ab/cd/<md5>"). Empty when fileUrl has one.
    QString fileExtension;

    // Set only for library posts: the downloaded file's path on disk and the
    // database row id, which paging uses as a stable cursor.
    QString localFilePath;
    qint64 libraryId = 0;
};

// File extensions played as video rather than decoded as an image.
inline bool isVideoExtension(const QString &extension)
{
    static const QStringList videoExtensions = {
        QStringLiteral("webm"), QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("mov"), QStringLiteral("mkv"), QStringLiteral("avi"),
    };
    return videoExtensions.contains(extension.toLower());
}

// By the extension of the URL's path; a query string does not count.
inline bool isVideoUrl(const QString &url)
{
    const QString path = QUrl(url).path();
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot < path.lastIndexOf(QLatin1Char('/')))
        return false;
    return isVideoExtension(path.mid(dot + 1));
}

// The post's file is a video, also when its URL has no extension and the
// site reported one separately (Shimmie2).
inline bool isVideoPost(const Post &post)
{
    return isVideoUrl(post.fileUrl) || isVideoExtension(post.fileExtension);
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

// A file to open, and whether it plays as a video.
struct MediaLink {
    QString url;
    bool video = false;
};

// What the zoom viewer opens; an empty url means nothing. A downloaded file
// always wins. A video sample plays first (it is smaller; on Danbooru it is
// also what an ugoira, a zip of frames, plays as). Then a video post plays
// its original. When a video post hides its original, its still sample or
// thumbnail is shown instead. An image post shows the sample before the
// original, since originals can be tens of megabytes.
inline MediaLink viewerMedia(const Post &post)
{
    const QString local = localImageUrl(post);
    if (!local.isEmpty())
        return {local, isVideoUrl(local)};
    if (isVideoUrl(post.sampleUrl))
        return {post.sampleUrl, true};
    if (isVideoPost(post) && !post.fileUrl.isEmpty())
        return {post.fileUrl, true};
    const QStringList candidates = {post.sampleUrl, post.fileUrl, post.previewUrl};
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && !isVideoUrl(candidate))
            return {candidate, false};
    }
    return {};
}

// What an animated grid thumbnail plays: the video the viewer would play,
// or the GIF original. An empty url means the post does not move.
inline MediaLink animatedThumbnailMedia(const Post &post)
{
    const MediaLink viewer = viewerMedia(post);
    if (viewer.video)
        return viewer;

    const QString file = post.localFilePath.isEmpty() ? post.fileUrl : localImageUrl(post);
    const bool gif = QUrl(file).path().endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)
        || (post.localFilePath.isEmpty() && post.fileExtension.compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0);
    if (file.isEmpty() || !gif)
        return {};
    return {file, false};
}

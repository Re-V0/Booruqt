// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"

// Gelbooru 0.1, which has no API: reads the HTML post list,
// index.php?page=post&s=list&tags=...&pid=OFFSET, one request per page.
// Covers: the booru.org network (<board>.booru.org) and self-hosted
// Gelbooru 0.1 sites.
//
// The list already has everything but the image address: each thumbnail's
// title holds the tags, "score:N" and "rating:Safe", and the image lives
// next to the thumbnail under a name derived from it:
//   thumbs.booru.org/<board>/thumbnails//<dir>/thumbnail_<name>
//   img.booru.org/<board>//images/<dir>/<name>
//   (self-hosted: <base>/thumbnails/<dir>/thumbnail_<name> and
//    <base>/images/<dir>/<name>)
// Width and height are not on the list and stay 0.
class GelbooruHtmlSource : public ISource {
public:
    GelbooruHtmlSource(QString name, QString baseUrl);

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("gelbooru-html"); }
    QString baseUrl() const override { return m_baseUrl; }

    // perPage is ignored: the site decides, and pid counts posts.
    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

    // The image address for a thumbnail address, or empty if it does not
    // follow the pattern above.
    static QString imageUrlForThumbnail(const QString &thumbnailUrl);

    // Posts per list page on Gelbooru 0.1 (booru.org: pid steps by 20).
    static constexpr int kPostsPerPage = 20;

private:
    QString m_name;
    QString m_baseUrl;
};

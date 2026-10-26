// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "GelbooruHtmlSource.h"
#include "../AppInfo.h"
#include "../HtmlEntities.h"

#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <utility>

namespace {

// The value of one attribute inside a tag's text, in "" or ''.
QString attribute(const QString &tag, const QString &name)
{
    const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*(\"([^\"]*)\"|'([^']*)')").arg(name),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(tag);
    if (!m.hasMatch())
        return {};
    return m.captured(2).isNull() ? m.captured(3) : m.captured(2);
}

QString ratingLetter(const QString &word)
{
    const QString lower = word.toLower();
    if (lower.startsWith(QLatin1Char('s')))
        return QStringLiteral("s");
    if (lower.startsWith(QLatin1Char('q')))
        return QStringLiteral("q");
    if (lower.startsWith(QLatin1Char('e')))
        return QStringLiteral("e");
    return {};
}

} // namespace

GelbooruHtmlSource::GelbooruHtmlSource(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest GelbooruHtmlSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    Q_UNUSED(perPage);
    QUrl url(m_baseUrl + QStringLiteral("/index.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("post"));
    query.addQueryItem(QStringLiteral("s"), QStringLiteral("list"));
    // An empty search lists nothing here; "all" lists everything.
    query.addQueryItem(QStringLiteral("tags"), tags.trimmed().isEmpty() ? QStringLiteral("all") : tags.trimmed());
    query.addQueryItem(QStringLiteral("pid"), QString::number(qMax(0, page - 1) * kPostsPerPage));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QString GelbooruHtmlSource::imageUrlForThumbnail(const QString &thumbnailUrl)
{
    QUrl url(thumbnailUrl);
    // ".../thumbnails/<dir>/thumbnail_<name>", with any number of slashes.
    static const QRegularExpression pattern(QStringLiteral("^(.*?)/thumbnails/+(\\d+)/thumbnail_([^/]+)$"));
    const QRegularExpressionMatch m = pattern.match(url.path());
    if (!m.hasMatch())
        return {};

    if (url.host() == QStringLiteral("thumbs.booru.org")) {
        // booru.org writes the double slash before "images" instead.
        url.setHost(QStringLiteral("img.booru.org"));
        url.setPath(m.captured(1) + QStringLiteral("//images/") + m.captured(2) + QLatin1Char('/') + m.captured(3));
    } else {
        url.setPath(m.captured(1) + QStringLiteral("/images/") + m.captured(2) + QLatin1Char('/') + m.captured(3));
    }
    return url.toString();
}

QVector<Post> GelbooruHtmlSource::parseSearchResponse(const QByteArray &data) const
{
    const QString page = QString::fromUtf8(data);
    const QUrl base(m_baseUrl + QLatin1Char('/'));
    QVector<Post> posts;
    QStringList seenIds;

    // A post link, then the thumbnail image inside it.
    static const QRegularExpression linkRe(
        QStringLiteral("<a\\b[^>]*href\\s*=\\s*[\"'][^\"']*s=view&(?:amp;)?id=(\\d+)[^\"']*[\"'][^>]*>\\s*(<img\\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = linkRe.globalMatch(page);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString image = m.captured(2);
        const QString src = decodeHtmlEntities(attribute(image, QStringLiteral("src")));
        if (src.isEmpty() || seenIds.contains(m.captured(1)))
            continue;
        seenIds.append(m.captured(1));

        Post p;
        p.id = m.captured(1);
        p.sourceName = m_name;
        p.previewUrl = base.resolved(QUrl(src)).toString();
        p.fileUrl = imageUrlForThumbnail(p.previewUrl);
        p.postPageUrl = m_baseUrl + QStringLiteral("/index.php?page=post&s=view&id=") + p.id;

        // " tag tag  score:0 rating:Safe"
        const QString title = decodeHtmlEntities(attribute(image, QStringLiteral("title")));
        for (const QString &word : title.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (word.startsWith(QStringLiteral("score:")))
                p.score = word.mid(6).toInt();
            else if (word.startsWith(QStringLiteral("rating:")))
                p.rating = ratingLetter(word.mid(7));
            else
                p.tags.append(word);
        }

        // Without a derivable image address the post can be viewed but not
        // downloaded; the thumbnail is not passed off as the file.
        posts.append(p);
    }
    return posts;
}

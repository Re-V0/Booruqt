// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ShimmieSource.h"
#include "../AppInfo.h"
#include "../HtmlEntities.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <algorithm>
#include <utility>

namespace {

// The API returns at most this many posts per request.
constexpr int kMaxPerPage = 100;

} // namespace

ShimmieSource::ShimmieSource(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest ShimmieSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/index.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), QStringLiteral("api/danbooru/find_posts"));
    query.addQueryItem(QStringLiteral("tags"), tags);
    // "page" is 1-based here; the API turns it into an offset of
    // (page - 1) * limit.
    query.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
    query.addQueryItem(QStringLiteral("limit"), QString::number(qMin(perPage, kMaxPerPage)));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QVector<Post> ShimmieSource::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("post"))
            continue;

        const QXmlStreamAttributes a = xml.attributes();
        Post p;
        p.id = a.value(QLatin1String("id")).toString();
        p.sourceName = m_name;
        p.tags = decodeHtmlEntities(a.value(QLatin1String("tags")).toString())
                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // The API has no ratings or scores and sends "?" and 0 for them.
        const QString rating = a.value(QLatin1String("rating")).toString();
        p.rating = rating == QStringLiteral("?") ? QString() : rating;
        p.score = a.value(QLatin1String("score")).toInt();
        p.width = a.value(QLatin1String("width")).toInt();
        p.height = a.value(QLatin1String("height")).toInt();
        p.fileUrl = a.value(QLatin1String("file_url")).toString();
        p.previewUrl = a.value(QLatin1String("preview_url")).toString();
        p.artistSourceUrl = a.value(QLatin1String("source")).toString();
        p.postPageUrl = m_baseUrl + QStringLiteral("/index.php?q=post/view/") + p.id;
        // Some sites serve files from a CDN path with no extension
        // ("/ab/cd/<md5>"); the original name still carries it.
        p.fileExtension = QFileInfo(a.value(QLatin1String("file_name")).toString()).suffix().toLower();

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.fileUrl;
        if (!p.id.isEmpty() && !p.fileUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest ShimmieSource::buildTagSuggestRequest(const QString &prefix) const
{
    QUrl url(m_baseUrl + QStringLiteral("/index.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), QStringLiteral("api/internal/autocomplete"));
    query.addQueryItem(QStringLiteral("s"), prefix);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QStringList ShimmieSource::parseTagSuggestions(const QByteArray &data) const
{
    // {"tag": {"newtag": null or "alias target", "count": N}, ...}; older
    // versions send {"tag": N}. Keys arrive sorted by name, so the order of
    // use is restored from the counts.
    struct Suggestion {
        QString tag;
        int count;
    };
    QVector<Suggestion> suggestions;
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        Suggestion s;
        s.tag = it.key();
        s.count = 0;
        if (it.value().isObject()) {
            const QJsonObject entry = it.value().toObject();
            const QString alias = entry.value(QStringLiteral("newtag")).toString();
            if (!alias.isEmpty())
                s.tag = alias;
            s.count = entry.value(QStringLiteral("count")).toInt();
        } else {
            s.count = it.value().toInt();
        }
        suggestions.append(s);
    }
    std::stable_sort(suggestions.begin(), suggestions.end(),
                     [](const Suggestion &a, const Suggestion &b) { return a.count > b.count; });

    QStringList tags;
    for (const Suggestion &s : suggestions) {
        if (!s.tag.isEmpty() && !tags.contains(s.tag))
            tags.append(s.tag);
    }
    return tags;
}

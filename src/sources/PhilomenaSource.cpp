// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "PhilomenaSource.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace {

// The API's own limit; a larger per_page is not honoured.
constexpr int kMaxPerPage = 50;

// Philomena tags contain spaces ("twilight sparkle"), while the rest of the
// app separates tags with spaces. At the API boundary "_" stands for a
// space: shown and typed as twilight_sparkle, sent as "twilight sparkle".
// A Philomena tag with a real "_" in it cannot be expressed; those are rare.
QString tagFromApi(const QString &tag)
{
    return QString(tag).replace(QLatin1Char(' '), QLatin1Char('_'));
}

QString tagToApi(const QString &tag)
{
    return QString(tag).replace(QLatin1Char('_'), QLatin1Char(' '));
}

} // namespace

PhilomenaSource::PhilomenaSource(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest PhilomenaSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/api/v1/json/search/images"));
    QUrlQuery query;
    QStringList apiTags;
    for (const QString &tag : tags.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        apiTags.append(tagToApi(tag));
    const QString commaTags = apiTags.join(QStringLiteral(", "));
    query.addQueryItem(QStringLiteral("q"), commaTags.isEmpty() ? QStringLiteral("*") : commaTags);
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    // Page-based paging, so a capped page size stays consistent across pages.
    query.addQueryItem(QStringLiteral("per_page"), QString::number(qMin(perPage, kMaxPerPage)));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QVector<Post> PhilomenaSource::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return posts;

    const QJsonArray arr = doc.object().value(QStringLiteral("images")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Post p;
        p.id = QString::number(o.value(QStringLiteral("id")).toInt());
        p.sourceName = m_name;
        for (const QJsonValue &t : o.value(QStringLiteral("tags")).toArray())
            p.tags.append(tagFromApi(t.toString()));
        p.score = o.value(QStringLiteral("score")).toInt();
        p.width = o.value(QStringLiteral("width")).toInt();
        p.height = o.value(QStringLiteral("height")).toInt();

        // Philomena has no single "rating" field - safety is expressed as
        // tags themselves (safe/suggestive/questionable/explicit/grimdark/...).
        static const QStringList ratingTags = {
            QStringLiteral("safe"), QStringLiteral("suggestive"),
            QStringLiteral("questionable"), QStringLiteral("explicit"),
            QStringLiteral("grimdark"), QStringLiteral("grotesque")
        };
        for (const QString &rt : ratingTags) {
            if (p.tags.contains(rt)) {
                p.rating = rt;
                break;
            }
        }

        const QJsonObject representations = o.value(QStringLiteral("representations")).toObject();
        p.previewUrl = representations.value(QStringLiteral("thumb")).toString();
        p.sampleUrl = representations.value(QStringLiteral("medium")).toString();
        p.fileUrl = o.value(QStringLiteral("view_url")).toString();
        p.artistSourceUrl = o.value(QStringLiteral("source_url")).toString();
        p.postPageUrl = m_baseUrl + QStringLiteral("/images/") + p.id;

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.sampleUrl.isEmpty() ? p.fileUrl : p.sampleUrl;
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest PhilomenaSource::buildTagSuggestRequest(const QString &prefix) const
{
    QUrl url(m_baseUrl + QStringLiteral("/api/v1/json/search/tags"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), tagToApi(prefix) + QLatin1Char('*'));
    query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("10"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QStringList PhilomenaSource::parseTagSuggestions(const QByteArray &data) const
{
    QStringList tags;
    const QJsonArray arr = QJsonDocument::fromJson(data).object().value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &v : arr) {
        const QString name = v.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            tags.append(tagFromApi(name));
    }
    return tags;
}

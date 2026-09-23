// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DanbooruSource.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

DanbooruSource::DanbooruSource(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest DanbooruSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/posts.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("tags"), tags);
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QVector<Post> DanbooruSource::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return posts;

    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        Post p;
        p.id = QString::number(o.value(QStringLiteral("id")).toInt());
        p.sourceName = m_name;
        p.tags = o.value(QStringLiteral("tag_string")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        p.rating = o.value(QStringLiteral("rating")).toString();
        p.score = o.value(QStringLiteral("score")).toInt();
        p.width = o.value(QStringLiteral("image_width")).toInt();
        p.height = o.value(QStringLiteral("image_height")).toInt();
        p.previewUrl = o.value(QStringLiteral("preview_file_url")).toString();
        p.sampleUrl = o.value(QStringLiteral("large_file_url")).toString();
        p.fileUrl = o.value(QStringLiteral("file_url")).toString();
        p.artistSourceUrl = o.value(QStringLiteral("source")).toString();
        p.postPageUrl = m_baseUrl + QStringLiteral("/posts/") + p.id;

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.sampleUrl.isEmpty() ? p.fileUrl : p.sampleUrl;
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest DanbooruSource::buildTagSuggestRequest(const QString &prefix) const
{
    QUrl url(m_baseUrl + QStringLiteral("/autocomplete.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("search[query]"), prefix);
    query.addQueryItem(QStringLiteral("search[type]"), QStringLiteral("tag_query"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QStringList DanbooruSource::parseTagSuggestions(const QByteArray &data) const
{
    QStringList tags;
    for (const QJsonValue &v : QJsonDocument::fromJson(data).array()) {
        const QString value = v.toObject().value(QStringLiteral("value")).toString();
        if (!value.isEmpty())
            tags.append(value);
    }
    return tags;
}

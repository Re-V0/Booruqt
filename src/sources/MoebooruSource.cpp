// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "MoebooruSource.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

MoebooruSource::MoebooruSource(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest MoebooruSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/post.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("tags"), tags);
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QVector<Post> MoebooruSource::parseSearchResponse(const QByteArray &data) const
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
        p.tags = o.value(QStringLiteral("tags")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        p.rating = o.value(QStringLiteral("rating")).toString();
        p.score = o.value(QStringLiteral("score")).toInt();
        p.width = o.value(QStringLiteral("width")).toInt();
        p.height = o.value(QStringLiteral("height")).toInt();
        p.previewUrl = o.value(QStringLiteral("preview_url")).toString();
        p.sampleUrl = o.value(QStringLiteral("sample_url")).toString();
        p.fileUrl = o.value(QStringLiteral("file_url")).toString();
        p.artistSourceUrl = o.value(QStringLiteral("source")).toString();
        p.postPageUrl = m_baseUrl + QStringLiteral("/post/show/") + p.id;

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.sampleUrl.isEmpty() ? p.fileUrl : p.sampleUrl;
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest MoebooruSource::buildTagSuggestRequest(const QString &prefix) const
{
    QUrl url(m_baseUrl + QStringLiteral("/tag.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), prefix);
    query.addQueryItem(QStringLiteral("order"), QStringLiteral("count"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QStringList MoebooruSource::parseTagSuggestions(const QByteArray &data) const
{
    QStringList tags;
    for (const QJsonValue &v : QJsonDocument::fromJson(data).array()) {
        const QString name = v.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            tags.append(name);
    }
    return tags;
}

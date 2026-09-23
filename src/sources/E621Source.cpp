// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "E621Source.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

E621Source::E621Source(QString name, QString baseUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl))
{
}

QNetworkRequest E621Source::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/posts.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("tags"), tags);
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
    url.setQuery(query);

    QNetworkRequest req(url);
    // e621 requires a descriptive UA identifying the application; a generic
    // browser-looking UA gets requests throttled/blocked by their API rules.
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

static QStringList flattenTagCategories(const QJsonObject &tagsObj)
{
    QStringList all;
    for (auto it = tagsObj.constBegin(); it != tagsObj.constEnd(); ++it) {
        for (const QJsonValue &tag : it.value().toArray())
            all.append(tag.toString());
    }
    return all;
}

QVector<Post> E621Source::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return posts;

    const QJsonArray arr = doc.object().value(QStringLiteral("posts")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Post p;
        p.id = QString::number(o.value(QStringLiteral("id")).toInt());
        p.sourceName = m_name;
        p.tags = flattenTagCategories(o.value(QStringLiteral("tags")).toObject());
        p.rating = o.value(QStringLiteral("rating")).toString();
        p.score = o.value(QStringLiteral("score")).toObject().value(QStringLiteral("total")).toInt();

        const QJsonObject file = o.value(QStringLiteral("file")).toObject();
        const QJsonObject preview = o.value(QStringLiteral("preview")).toObject();
        const QJsonObject sample = o.value(QStringLiteral("sample")).toObject();

        p.width = file.value(QStringLiteral("width")).toInt();
        p.height = file.value(QStringLiteral("height")).toInt();
        p.fileUrl = file.value(QStringLiteral("url")).toString();
        p.previewUrl = preview.value(QStringLiteral("url")).toString();
        p.sampleUrl = sample.value(QStringLiteral("url")).toString();

        const QJsonArray sources = o.value(QStringLiteral("sources")).toArray();
        if (!sources.isEmpty())
            p.artistSourceUrl = sources.first().toString();

        p.postPageUrl = m_baseUrl + QStringLiteral("/posts/") + p.id;

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.sampleUrl.isEmpty() ? p.fileUrl : p.sampleUrl;
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest E621Source::buildTagSuggestRequest(const QString &prefix) const
{
    QUrl url(m_baseUrl + QStringLiteral("/tags/autocomplete.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("search[name_matches]"), prefix);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    return req;
}

QStringList E621Source::parseTagSuggestions(const QByteArray &data) const
{
    QStringList tags;
    for (const QJsonValue &v : QJsonDocument::fromJson(data).array()) {
        const QString name = v.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            tags.append(name);
    }
    return tags;
}

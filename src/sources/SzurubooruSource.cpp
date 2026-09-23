// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SzurubooruSource.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

SzurubooruSource::SzurubooruSource(QString name, QString baseUrl, QString apiKey, QString userId)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl)),
      m_apiKey(std::move(apiKey)), m_userId(std::move(userId))
{
}

QNetworkRequest SzurubooruSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QUrl url(m_baseUrl + QStringLiteral("/api/posts/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), tags);
    query.addQueryItem(QStringLiteral("offset"), QString::number(qMax(0, (page - 1) * perPage)));
    query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    if (!m_apiKey.isEmpty() && !m_userId.isEmpty()) {
        const QByteArray token = (m_userId + QLatin1Char(':') + m_apiKey).toUtf8().toBase64();
        req.setRawHeader("Authorization", "Token " + token);
    }
    return req;
}

QVector<Post> SzurubooruSource::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return posts;

    const QJsonArray arr = doc.object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Post p;
        p.id = QString::number(o.value(QStringLiteral("id")).toInt());
        p.sourceName = m_name;

        for (const QJsonValue &tagVal : o.value(QStringLiteral("tags")).toArray()) {
            const QJsonArray names = tagVal.toObject().value(QStringLiteral("names")).toArray();
            if (!names.isEmpty())
                p.tags.append(names.first().toString());
        }

        p.rating = o.value(QStringLiteral("safety")).toString();
        p.score = o.value(QStringLiteral("score")).toInt();
        p.width = o.value(QStringLiteral("canvasWidth")).toInt();
        p.height = o.value(QStringLiteral("canvasHeight")).toInt();
        p.artistSourceUrl = o.value(QStringLiteral("source")).toString();

        // URLs are relative to the instance root, not the /api prefix.
        const QUrl base(m_baseUrl);
        const QString contentPath = o.value(QStringLiteral("contentUrl")).toString();
        const QString thumbPath = o.value(QStringLiteral("thumbnailUrl")).toString();
        p.fileUrl = contentPath.isEmpty() ? QString() : base.resolved(QUrl(contentPath)).toString();
        p.previewUrl = thumbPath.isEmpty() ? QString() : base.resolved(QUrl(thumbPath)).toString();
        p.postPageUrl = m_baseUrl + QStringLiteral("/post/") + p.id;

        if (p.previewUrl.isEmpty())
            p.previewUrl = p.fileUrl;
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

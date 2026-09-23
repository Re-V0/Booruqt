// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "GelbooruSource.h"
#include "../HtmlEntities.h"
#include "../AppInfo.h"

#include <utility>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QJsonValue>
#include <QXmlStreamReader>

GelbooruSource::GelbooruSource(QString name, QString baseUrl, QString apiKey, QString userId,
                                QString cookie, QString userAgent, QString htmlBridgeUrl)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl)),
      m_apiKey(std::move(apiKey)), m_userId(std::move(userId)),
      m_cookie(std::move(cookie)), m_userAgent(std::move(userAgent)),
      m_htmlBridgeUrl(std::move(htmlBridgeUrl))
{
}

QNetworkRequest GelbooruSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    if (!m_htmlBridgeUrl.isEmpty()) {
        QUrl url(m_htmlBridgeUrl + QStringLiteral("/scrape_gelbooru_html"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("base_url"), m_baseUrl);
        query.addQueryItem(QStringLiteral("tags"), tags);
        query.addQueryItem(QStringLiteral("page"), QString::number(page));
        query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
        url.setQuery(query);
        return QNetworkRequest(url);
    }

    QUrl url(m_baseUrl + QStringLiteral("/index.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
    query.addQueryItem(QStringLiteral("s"), QStringLiteral("post"));
    query.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
    query.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("tags"), tags);
    // dapi pagination is 0-indexed
    query.addQueryItem(QStringLiteral("pid"), QString::number(qMax(0, page - 1)));
    query.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
    if (!m_apiKey.isEmpty())
        query.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    if (!m_userId.isEmpty())
        query.addQueryItem(QStringLiteral("user_id"), m_userId);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  m_userAgent.isEmpty() ? blairqtUserAgent() : m_userAgent);
    if (!m_cookie.isEmpty())
        req.setRawHeader("Cookie", m_cookie.toUtf8());
    return req;
}

// Shared by JSON/XML responses; falls back to directory+image+hash URLs
// for legacy forks that don't send file_url/preview_url/sample_url.
static Post postFromAttributeMap(const QString &sourceName, const QString &baseUrl,
                                  const QHash<QString, QString> &a)
{
    Post p;
    p.id = a.value(QStringLiteral("id"));
    p.sourceName = sourceName;
    // This engine HTML-escapes tags ("baldur&#039;s_gate_3").
    p.tags = decodeHtmlEntities(a.value(QStringLiteral("tags"))).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    p.rating = a.value(QStringLiteral("rating"));
    p.score = a.value(QStringLiteral("score")).toInt();
    p.width = a.value(QStringLiteral("width")).toInt();
    p.height = a.value(QStringLiteral("height")).toInt();
    p.previewUrl = a.value(QStringLiteral("preview_url"));
    p.sampleUrl = a.value(QStringLiteral("sample_url"));
    p.fileUrl = a.value(QStringLiteral("file_url"));
    p.artistSourceUrl = a.value(QStringLiteral("source"));
    p.postPageUrl = baseUrl + QStringLiteral("/index.php?page=post&s=view&id=") + p.id;

    const QString directory = a.value(QStringLiteral("directory"));
    const QString image = a.value(QStringLiteral("image"));
    const QString hash = a.value(QStringLiteral("hash"));
    const bool hasSample = a.value(QStringLiteral("sample")) == QStringLiteral("true")
        || a.value(QStringLiteral("sample")) == QStringLiteral("1");

    if (p.fileUrl.isEmpty() && !directory.isEmpty() && !image.isEmpty())
        p.fileUrl = baseUrl + QStringLiteral("/images/") + directory + QLatin1Char('/') + image;
    if (p.previewUrl.isEmpty() && !directory.isEmpty() && !hash.isEmpty()) {
        p.previewUrl = baseUrl + QStringLiteral("/thumbnails/") + directory
            + QStringLiteral("/thumbnail_") + hash + QStringLiteral(".jpg");
    }
    if (p.sampleUrl.isEmpty() && hasSample && !directory.isEmpty() && !hash.isEmpty()) {
        p.sampleUrl = baseUrl + QStringLiteral("/samples/") + directory
            + QStringLiteral("/sample_") + hash + QStringLiteral(".jpg");
    }

    if (p.previewUrl.isEmpty())
        p.previewUrl = p.sampleUrl.isEmpty() ? p.fileUrl : p.sampleUrl;
    return p;
}

static QHash<QString, QString> attributeMapFromJsonObject(const QJsonObject &o)
{
    QHash<QString, QString> a;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QJsonValue &v = it.value();
        // Stringify: bool on some mirrors, string on others.
        a.insert(it.key(), v.isBool() ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                       : v.toVariant().toString());
    }
    return a;
}

QVector<Post> GelbooruSource::parseSearchResponse(const QByteArray &data) const
{
    QVector<Post> posts;

    // Some mirrors ignore json=1 and always answer with XML.
    const QByteArray trimmed = data.trimmed();
    if (trimmed.startsWith("<?xml") || trimmed.startsWith("<posts")) {
        QXmlStreamReader xml(data);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("post")) {
                QHash<QString, QString> attrs;
                for (const QXmlStreamAttribute &attr : xml.attributes())
                    attrs.insert(attr.name().toString(), attr.value().toString());
                Post p = postFromAttributeMap(m_name, m_baseUrl, attrs);
                if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
                    posts.append(p);
            }
        }
        return posts;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);

    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        // Some mirrors wrap results in {"post"/"posts": [...] or one object}.
        const QJsonObject root = doc.object();
        QJsonValue postsValue = root.contains(QStringLiteral("post"))
            ? root.value(QStringLiteral("post"))
            : root.value(QStringLiteral("posts"));
        if (postsValue.isArray())
            arr = postsValue.toArray();
        else if (postsValue.isObject())
            arr.append(postsValue);
    }

    for (const QJsonValue &v : arr) {
        Post p = postFromAttributeMap(m_name, m_baseUrl, attributeMapFromJsonObject(v.toObject()));
        if (!p.fileUrl.isEmpty() || !p.previewUrl.isEmpty())
            posts.append(p);
    }
    return posts;
}

QNetworkRequest GelbooruSource::buildTagSuggestRequest(const QString &prefix) const
{
    if (!m_htmlBridgeUrl.isEmpty())
        return QNetworkRequest(); // scraped sites have no autocomplete endpoint

    QUrl url(m_baseUrl + QStringLiteral("/autocomplete.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), prefix);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  m_userAgent.isEmpty() ? blairqtUserAgent() : m_userAgent);
    if (!m_cookie.isEmpty())
        req.setRawHeader("Cookie", m_cookie.toUtf8());
    return req;
}

QStringList GelbooruSource::parseTagSuggestions(const QByteArray &data) const
{
    QStringList tags;
    for (const QJsonValue &v : QJsonDocument::fromJson(data).array()) {
        const QString value = decodeHtmlEntities(v.toObject().value(QStringLiteral("value")).toString());
        if (!value.isEmpty())
            tags.append(value);
    }
    return tags;
}

bool GelbooruSource::supportsTagExclusion() const
{
    // dapi honours "-tag"; the scrape bridge only forwards plain tags.
    return m_htmlBridgeUrl.isEmpty();
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"

// Gelbooru dapi flavor. cookie/userAgent: manual Cloudflare bypass.
class GelbooruSource : public ISource {
public:
    // htmlBridgeUrl: for API-less sites, scraped via a local Python bridge.
    GelbooruSource(QString name, QString baseUrl, QString apiKey = {}, QString userId = {},
                   QString cookie = {}, QString userAgent = {}, QString htmlBridgeUrl = {});

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("gelbooru"); }
    QString baseUrl() const override { return m_baseUrl; }

    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

    bool supportsTagExclusion() const override;

    QNetworkRequest buildTagSuggestRequest(const QString &prefix) const override;
    QStringList parseTagSuggestions(const QByteArray &data) const override;

private:
    QString m_name;
    QString m_baseUrl;
    QString m_apiKey;
    QString m_userId;
    QString m_cookie;
    QString m_userAgent;
    QString m_htmlBridgeUrl;
};

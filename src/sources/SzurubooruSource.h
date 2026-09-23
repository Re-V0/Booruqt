// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"

// Szurubooru flavor: a separate REST engine for self-hosted boorus.
class SzurubooruSource : public ISource {
public:
    SzurubooruSource(QString name, QString baseUrl, QString apiKey = {}, QString userId = {});

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("szurubooru"); }
    QString baseUrl() const override { return m_baseUrl; }

    // supportsTagExclusion() stays false on purpose: szurubooru's syntax does
    // document "-tag", but there was no public instance to verify it against,
    // and guessing wrong would silently break every search on this flavor.
    // The blacklist still applies, just by filtering the returned posts.
    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

private:
    QString m_name;
    QString m_baseUrl;
    QString m_apiKey;
    QString m_userId;
};

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"

// e621 flavor (e621.net, e926.net). Tags come grouped by category.
class E621Source : public ISource {
public:
    E621Source(QString name, QString baseUrl);

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("e621"); }
    QString baseUrl() const override { return m_baseUrl; }

    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

    // Verified against the live API: "-tag" excludes server-side.
    bool supportsTagExclusion() const override { return true; }

    QNetworkRequest buildTagSuggestRequest(const QString &prefix) const override;
    QStringList parseTagSuggestions(const QByteArray &data) const override;

private:
    QString m_name;
    QString m_baseUrl;
};

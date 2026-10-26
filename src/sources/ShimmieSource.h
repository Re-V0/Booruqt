// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"

// Shimmie2, through its Danbooru-compatible API (the danbooru_api
// extension): index.php?q=api/danbooru/find_posts&tags=...&page=N&limit=M,
// answering XML. The index.php?q= form works whether or not the site has
// "nice URLs" turned on. Covers: rule34.paheal.net and other Shimmie2 sites
// with that extension enabled.
class ShimmieSource : public ISource {
public:
    ShimmieSource(QString name, QString baseUrl);

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("shimmie"); }
    QString baseUrl() const override { return m_baseUrl; }

    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

    // Shimmie2's search treats "-tag" as "without tag".
    bool supportsTagExclusion() const override { return true; }

    QNetworkRequest buildTagSuggestRequest(const QString &prefix) const override;
    QStringList parseTagSuggestions(const QByteArray &data) const override;

private:
    QString m_name;
    QString m_baseUrl;
};

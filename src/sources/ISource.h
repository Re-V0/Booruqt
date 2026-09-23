// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QNetworkRequest>
#include <QVector>
#include <QString>
#include "../Post.h"

// Common interface each booru "flavor" (Danbooru, Gelbooru, Moebooru, ...) implements.
class ISource {
public:
    virtual ~ISource() = default;

    virtual QString name() const = 0;
    virtual QString flavor() const = 0;
    virtual QString baseUrl() const = 0;

    // page is always 1-indexed; implementations convert as needed.
    virtual QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const = 0;
    virtual QVector<Post> parseSearchResponse(const QByteArray &data) const = 0;

    // True if the site honours "-tag" in a search, so the blacklist can be
    // pushed server-side instead of only filtering what came back.
    virtual bool supportsTagExclusion() const { return false; }

    // True when the two calls above can take a long time - a plugin spawns a
    // process - so the caller must run them off the GUI thread. Both stay
    // synchronous; only the caller decides where they run.
    virtual bool mayBlock() const { return false; }

    // Tag autocomplete; an invalid request means the flavor has no endpoint.
    virtual QNetworkRequest buildTagSuggestRequest(const QString &prefix) const
    {
        Q_UNUSED(prefix);
        return QNetworkRequest();
    }
    virtual QStringList parseTagSuggestions(const QByteArray &data) const
    {
        Q_UNUSED(data);
        return {};
    }
};

// Appends the blacklist as "-tag" terms when the source honours them.
// Careful: supportsTagExclusion() may be expensive (a plugin asks its script),
// so for a mayBlock() source this belongs on a worker thread, not the GUI one.
inline QString searchTagsWithExclusions(const ISource &source, const QString &tags,
                                        const QStringList &blacklisted)
{
    if (blacklisted.isEmpty() || !source.supportsTagExclusion())
        return tags;

    QString result = tags;
    for (const QString &tag : blacklisted)
        result += QStringLiteral(" -") + tag;
    return result;
}

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
#include <QStringList>
#include <QUrl>
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

    // The most terms one search may carry, the user's own included; 0 when
    // the site has no such limit. e621 answers HTTP 422 above 40.
    virtual int maxSearchTerms() const { return 0; }

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

// Text typed by the user (tags, a tag prefix, a key) as the value of a URL
// query item. QUrlQuery takes a value as already encoded wherever it could
// be: it sends "+" as it is, which a server reads as a space - "6+girls"
// found nothing - and it keeps a literal "%2B" instead of escaping the "%".
// The characters left alone are the ones tag syntax uses and a query allows.
inline QString queryValue(const QString &text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text, ":,()*!~/@"));
}

// The longest tag string a search sends. A blacklist of hundreds of tags
// would otherwise grow the address past what servers accept (HTTP 414).
constexpr int kMaxSearchTagsLength = 1500;

// Appends the blacklist as "-tag" terms when the source honours them, as
// many as the site's term limit and the address length allow. The rest is
// no loss: results are filtered against the whole blacklist afterwards.
// Careful: supportsTagExclusion() may be expensive (a plugin asks its script),
// so for a mayBlock() source this belongs on a worker thread, not the GUI one.
inline QString searchTagsWithExclusions(const ISource &source, const QString &tags,
                                        const QStringList &blacklisted)
{
    if (blacklisted.isEmpty() || !source.supportsTagExclusion())
        return tags;

    const int termLimit = source.maxSearchTerms();
    int terms = int(tags.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());

    QString result = tags;
    for (const QString &tag : blacklisted) {
        if (termLimit > 0 && terms >= termLimit)
            break;
        if (result.size() + tag.size() + 2 > kMaxSearchTagsLength)
            break;
        result += QStringLiteral(" -") + tag;
        ++terms;
    }
    return result;
}

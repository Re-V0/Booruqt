// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Blacklist.h"

#include <QSettings>
#include <QSet>
#include <QHash>
#include <QUrl>

namespace {

constexpr auto kGlobalKey = "blacklist/global";
constexpr auto kLibraryKey = "blacklist/applyToLibrary";
constexpr auto kSourceGroup = "blacklistPerSource";

// A source name may contain "/", which QSettings would read as a subgroup.
QString sourceKey(const QString &sourceName)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(sourceName));
}

// effectiveTags() runs per source per page and per reply; without this it
// would hit QSettings in the middle of every search round.
QHash<QString, QStringList> &effectiveCache()
{
    static QHash<QString, QStringList> cache;
    return cache;
}

QStringList normalize(const QStringList &tags)
{
    QStringList out;
    for (const QString &raw : tags) {
        const QString tag = raw.trimmed().toLower();
        if (!tag.isEmpty() && !out.contains(tag))
            out.append(tag);
    }
    return out;
}

} // namespace

namespace Blacklist {

QStringList globalTags()
{
    QSettings settings;
    return settings.value(QLatin1String(kGlobalKey)).toStringList();
}

void setGlobalTags(const QStringList &tags)
{
    QSettings settings;
    settings.setValue(QLatin1String(kGlobalKey), normalize(tags));
    effectiveCache().clear();
}

QStringList tagsForSource(const QString &sourceName)
{
    if (sourceName.isEmpty())
        return {};
    QSettings settings;
    settings.beginGroup(QLatin1String(kSourceGroup));
    // Falls back to the plain name so lists saved before the encoding survive.
    QStringList tags = settings.value(sourceKey(sourceName)).toStringList();
    if (tags.isEmpty())
        tags = settings.value(sourceName).toStringList();
    settings.endGroup();
    return tags;
}

void setTagsForSource(const QString &sourceName, const QStringList &tags)
{
    if (sourceName.isEmpty())
        return;
    QSettings settings;
    settings.beginGroup(QLatin1String(kSourceGroup));
    // Drop a pre-encoding entry, but only when the raw name is a plain key:
    // remove() on a name containing "/" would wipe a whole settings group.
    if (sourceName == sourceKey(sourceName))
        settings.remove(sourceName);
    const QStringList cleaned = normalize(tags);
    if (cleaned.isEmpty())
        settings.remove(sourceKey(sourceName));
    else
        settings.setValue(sourceKey(sourceName), cleaned);
    settings.endGroup();
    effectiveCache().clear();
}

QStringList effectiveTags(const QString &sourceName)
{
    const auto cached = effectiveCache().constFind(sourceName);
    if (cached != effectiveCache().constEnd())
        return cached.value();

    const QStringList tags = normalize(globalTags() + tagsForSource(sourceName));
    effectiveCache().insert(sourceName, tags);
    return tags;
}

bool appliesToLibrary()
{
    QSettings settings;
    return settings.value(QLatin1String(kLibraryKey), false).toBool();
}

void setAppliesToLibrary(bool enabled)
{
    QSettings settings;
    settings.setValue(QLatin1String(kLibraryKey), enabled);
}

bool blocks(const Post &post, const QStringList &blacklisted)
{
    if (blacklisted.isEmpty())
        return false;
    for (const QString &tag : post.tags) {
        if (blacklisted.contains(tag.toLower()))
            return true;
    }
    return false;
}

} // namespace Blacklist

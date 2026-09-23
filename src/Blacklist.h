// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QStringList>
#include "Post.h"

// Tags the user never wants to see. Stored in QSettings: one global list plus
// an optional per-source list, since sites tag the same thing differently.
namespace Blacklist {

QStringList globalTags();
void setGlobalTags(const QStringList &tags);

QStringList tagsForSource(const QString &sourceName);
void setTagsForSource(const QString &sourceName, const QStringList &tags);

// Global plus this source's own list, lowercased and deduplicated.
QStringList effectiveTags(const QString &sourceName);

// Exact tag match only - a substring rule would hide unrelated posts.
bool blocks(const Post &post, const QStringList &blacklisted);

// Off by default: downloaded files are the user's own, hiding them surprises.
bool appliesToLibrary();
void setAppliesToLibrary(bool enabled);

} // namespace Blacklist

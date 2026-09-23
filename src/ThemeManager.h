// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>
#include <QVector>

// Optional JSON-configured UI theme; absent file = untouched defaults.
namespace ThemeManager {

QString configFilePath();

// Applies theme.json live. Writes a starter file if none exists yet.
void applyTheme();

// "defaultIconSize" from theme.json, or -1 if absent/invalid.
int defaultIconSize();

// One curated color scheme from resources/curated_themes.json.
struct CuratedTheme {
    QString name;
    QString colorsJson; // pre-serialized "colors" object
};

QVector<CuratedTheme> curatedThemes();

// Applies a curated theme by name; no-op if it doesn't exist.
void applyCuratedTheme(const QString &name);

// Restores the real system palette captured before any customization.
void applyDefaultTheme();

} // namespace ThemeManager

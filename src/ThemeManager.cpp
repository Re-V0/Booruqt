// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ThemeManager.h"

#include <QApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPalette>
#include <QColor>

namespace {

struct RoleEntry {
    const char *key;
    QPalette::ColorRole role;
};

// Covers the roles real widgets actually draw with.
constexpr RoleEntry kRoles[] = {
    {"window", QPalette::Window},
    {"windowText", QPalette::WindowText},
    {"base", QPalette::Base},
    {"alternateBase", QPalette::AlternateBase},
    {"text", QPalette::Text},
    {"button", QPalette::Button},
    {"buttonText", QPalette::ButtonText},
    {"brightText", QPalette::BrightText},
    {"highlight", QPalette::Highlight},
    {"highlightedText", QPalette::HighlightedText},
    {"link", QPalette::Link},
    {"linkVisited", QPalette::LinkVisited},
    {"placeholderText", QPalette::PlaceholderText},
    {"toolTipBase", QPalette::ToolTipBase},
    {"toolTipText", QPalette::ToolTipText},
    {"light", QPalette::Light},
    {"midlight", QPalette::Midlight},
    {"dark", QPalette::Dark},
    {"mid", QPalette::Mid},
    {"shadow", QPalette::Shadow},
};

QJsonObject paletteToJson(const QPalette &palette)
{
    QJsonObject colors;
    for (const RoleEntry &entry : kRoles)
        colors[QLatin1String(entry.key)] = palette.color(entry.role).name(QColor::HexArgb);
    return colors;
}

QJsonObject readRoot()
{
    QFile file(ThemeManager::configFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void writeRoot(const QJsonObject &root)
{
    QFile file(ThemeManager::configFilePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(root).toJson());
}

} // namespace

QString ThemeManager::configFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/theme.json");
}

void ThemeManager::applyTheme()
{
    const QString path = configFilePath();
    if (!QFile::exists(path)) {
        // Seeds a starter file with real values to tweak, not a blank one.
        const QJsonObject snapshot = paletteToJson(QApplication::palette());
        QJsonObject root;
        root[QStringLiteral("colors")] = snapshot;
        root[QStringLiteral("systemDefaultColors")] = snapshot;
        root[QStringLiteral("defaultIconSize")] = 220;
        writeRoot(root);
        return;
    }

    QJsonObject root = readRoot();

    // Backfill: capture fresh, never trust stored "colors" (may be customized).
    if (!root.contains(QStringLiteral("systemDefaultColors"))) {
        root[QStringLiteral("systemDefaultColors")] = paletteToJson(QApplication::palette());
        writeRoot(root);
    }

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    if (colors.isEmpty())
        return;

    QPalette palette = QApplication::palette();
    for (const RoleEntry &entry : kRoles) {
        const QJsonValue v = colors.value(QLatin1String(entry.key));
        if (!v.isString())
            continue;
        const QColor color(v.toString());
        if (color.isValid())
            palette.setColor(entry.role, color);
    }
    QApplication::setPalette(palette);
}

int ThemeManager::defaultIconSize()
{
    const QJsonValue v = readRoot().value(QStringLiteral("defaultIconSize"));
    return v.isDouble() ? v.toInt() : -1;
}

QVector<ThemeManager::CuratedTheme> ThemeManager::curatedThemes()
{
    QVector<CuratedTheme> themes;

    QFile file(QStringLiteral(":/curated_themes.json"));
    if (!file.open(QIODevice::ReadOnly))
        return themes;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return themes;

    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QJsonObject colors = o.value(QStringLiteral("colors")).toObject();
        if (name.isEmpty() || colors.isEmpty())
            continue;
        themes.append({name, QString::fromUtf8(QJsonDocument(colors).toJson(QJsonDocument::Compact))});
    }
    return themes;
}

void ThemeManager::applyCuratedTheme(const QString &name)
{
    for (const CuratedTheme &theme : curatedThemes()) {
        if (theme.name != name)
            continue;

        QJsonObject root = readRoot();
        root[QStringLiteral("colors")] = QJsonDocument::fromJson(theme.colorsJson.toUtf8()).object();
        writeRoot(root);

        applyTheme();
        return;
    }
}

void ThemeManager::applyDefaultTheme()
{
    QJsonObject root = readRoot();
    const QJsonObject defaults = root.value(QStringLiteral("systemDefaultColors")).toObject();
    if (defaults.isEmpty())
        return;

    root[QStringLiteral("colors")] = defaults;
    writeRoot(root);
    applyTheme();
}

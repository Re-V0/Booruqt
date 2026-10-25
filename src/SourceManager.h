// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QVector>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QSet>
#include "sources/ISource.h"

// Loads built-in and custom booru sources, instantiates the right flavor.
class SourceManager {
public:
    SourceManager();

    const QVector<QSharedPointer<ISource>> &sources() const { return m_sources; }
    QSharedPointer<ISource> byName(const QString &name) const;

    // cookie/userAgent/htmlBridgeUrl are gelbooru-only; scriptPath is python-only.
    static QSharedPointer<ISource> createSource(const QString &name, const QString &flavor,
                                                 const QString &baseUrl, const QString &apiKey = {},
                                                 const QString &userId = {}, const QString &cookie = {},
                                                 const QString &userAgent = {}, const QString &htmlBridgeUrl = {},
                                                 const QString &scriptPath = {});

    static QStringList knownFlavors();

    // Adds a source and persists it to custom_sources.json.
    void addCustomSource(const QString &name, const QString &flavor, const QString &baseUrl,
                          const QString &apiKey = {}, const QString &userId = {},
                          const QString &cookie = {}, const QString &userAgent = {},
                          const QString &htmlBridgeUrl = {}, const QString &scriptPath = {});

    // Custom sources are deleted outright; built-ins are hidden instead.
    void removeSource(const QString &name);
    void restoreDefaultSources();

private:
    // onlyName: load just the built-in with that name (if not hidden).
    void loadDefaults(const QString &onlyName = {});
    void loadUserSources();
    void saveUserSources() const;
    void loadHiddenNames();
    void saveHiddenNames() const;
    // Replaces any existing source with the same display name.
    void upsertSource(const QSharedPointer<ISource> &source);

    static QString userSourcesFilePath();
    static QString hiddenSourcesFilePath();
    static QString appDataFilePath(const QString &fileName);
    // Index into m_userSourcesJson of the entry named `name`, or -1 -
    // shared by addCustomSource() (replace-if-exists) and removeSource().
    int indexOfUserSourceJson(const QString &name) const;

    QVector<QSharedPointer<ISource>> m_sources;
    QJsonArray m_userSourcesJson;
    QSet<QString> m_hiddenBuiltinNames;
};

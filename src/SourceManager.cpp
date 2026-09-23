// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SourceManager.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDebug>

#include "sources/DanbooruSource.h"
#include "sources/GelbooruSource.h"
#include "sources/MoebooruSource.h"
#include "sources/E621Source.h"
#include "sources/PhilomenaSource.h"
#include "sources/SzurubooruSource.h"
#include "sources/PythonPluginSource.h"

SourceManager::SourceManager()
{
    loadHiddenNames();
    loadDefaults();
    loadUserSources();
}

QSharedPointer<ISource> SourceManager::createSource(const QString &name, const QString &flavor,
                                                      const QString &baseUrl, const QString &apiKey,
                                                      const QString &userId, const QString &cookie,
                                                      const QString &userAgent, const QString &htmlBridgeUrl,
                                                      const QString &scriptPath)
{
    if (flavor == QStringLiteral("danbooru"))
        return QSharedPointer<DanbooruSource>::create(name, baseUrl);
    if (flavor == QStringLiteral("gelbooru")) {
        return QSharedPointer<GelbooruSource>::create(name, baseUrl, apiKey, userId, cookie,
                                                        userAgent, htmlBridgeUrl);
    }
    if (flavor == QStringLiteral("moebooru"))
        return QSharedPointer<MoebooruSource>::create(name, baseUrl);
    if (flavor == QStringLiteral("e621"))
        return QSharedPointer<E621Source>::create(name, baseUrl);
    if (flavor == QStringLiteral("philomena"))
        return QSharedPointer<PhilomenaSource>::create(name, baseUrl);
    if (flavor == QStringLiteral("szurubooru"))
        return QSharedPointer<SzurubooruSource>::create(name, baseUrl, apiKey, userId);
    if (flavor == QStringLiteral("python")) {
        if (scriptPath.isEmpty())
            return {};
        return QSharedPointer<PythonPluginSource>::create(name, baseUrl, scriptPath);
    }
    return {};
}

QStringList SourceManager::knownFlavors()
{
    return {
        QStringLiteral("danbooru"),
        QStringLiteral("gelbooru"),
        QStringLiteral("moebooru"),
        QStringLiteral("e621"),
        QStringLiteral("philomena"),
        QStringLiteral("szurubooru"),
    };
}

void SourceManager::loadDefaults()
{
    QFile file(QStringLiteral(":/sources.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "SourceManager: could not open embedded sources.json";
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        qWarning() << "SourceManager: sources.json is not a JSON array";
        return;
    }

    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString flavor = o.value(QStringLiteral("flavor")).toString();
        const QString baseUrl = o.value(QStringLiteral("baseUrl")).toString();
        if (name.isEmpty() || flavor.isEmpty() || baseUrl.isEmpty())
            continue;
        if (m_hiddenBuiltinNames.contains(name))
            continue;

        const QString apiKey = o.value(QStringLiteral("apiKey")).toString();
        const QString userId = o.value(QStringLiteral("userId")).toString();
        const QString cookie = o.value(QStringLiteral("cookie")).toString();
        const QString userAgent = o.value(QStringLiteral("userAgent")).toString();
        const QString htmlBridgeUrl = o.value(QStringLiteral("htmlBridgeUrl")).toString();
        QSharedPointer<ISource> source = createSource(name, flavor, baseUrl, apiKey, userId, cookie,
                                                        userAgent, htmlBridgeUrl);
        if (!source) {
            qWarning() << "SourceManager: unknown flavor" << flavor << "for" << name;
            continue;
        }
        m_sources.append(source);
    }
}

QString SourceManager::appDataFilePath(const QString &fileName)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + fileName;
}

QString SourceManager::userSourcesFilePath()
{
    return appDataFilePath(QStringLiteral("custom_sources.json"));
}

void SourceManager::loadUserSources()
{
    QFile file(userSourcesFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;

    m_userSourcesJson = doc.array();
    for (const QJsonValue &v : m_userSourcesJson) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString flavor = o.value(QStringLiteral("flavor")).toString();
        const QString baseUrl = o.value(QStringLiteral("baseUrl")).toString();
        const QString apiKey = o.value(QStringLiteral("apiKey")).toString();
        const QString userId = o.value(QStringLiteral("userId")).toString();
        const QString cookie = o.value(QStringLiteral("cookie")).toString();
        const QString userAgent = o.value(QStringLiteral("userAgent")).toString();
        const QString htmlBridgeUrl = o.value(QStringLiteral("htmlBridgeUrl")).toString();
        const QString scriptPath = o.value(QStringLiteral("scriptPath")).toString();
        if (name.isEmpty() || flavor.isEmpty() || baseUrl.isEmpty())
            continue;

        QSharedPointer<ISource> source = createSource(name, flavor, baseUrl, apiKey, userId, cookie,
                                                        userAgent, htmlBridgeUrl, scriptPath);
        if (source)
            upsertSource(source);
    }
}

void SourceManager::upsertSource(const QSharedPointer<ISource> &source)
{
    for (auto &existing : m_sources) {
        if (existing->name() == source->name()) {
            existing = source;
            return;
        }
    }
    m_sources.append(source);
}

void SourceManager::saveUserSources() const
{
    QFile file(userSourcesFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "SourceManager: could not write" << file.fileName();
        return;
    }
    file.write(QJsonDocument(m_userSourcesJson).toJson());
    // Holds API keys and session cookies.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void SourceManager::addCustomSource(const QString &name, const QString &flavor, const QString &baseUrl,
                                     const QString &apiKey, const QString &userId,
                                     const QString &cookie, const QString &userAgent,
                                     const QString &htmlBridgeUrl, const QString &scriptPath)
{
    QSharedPointer<ISource> source = createSource(name, flavor, baseUrl, apiKey, userId, cookie,
                                                    userAgent, htmlBridgeUrl, scriptPath);
    if (!source)
        return;

    upsertSource(source);

    QJsonObject o;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("flavor")] = flavor;
    o[QStringLiteral("baseUrl")] = baseUrl;
    if (!apiKey.isEmpty())
        o[QStringLiteral("apiKey")] = apiKey;
    if (!userId.isEmpty())
        o[QStringLiteral("userId")] = userId;
    if (!cookie.isEmpty())
        o[QStringLiteral("cookie")] = cookie;
    if (!userAgent.isEmpty())
        o[QStringLiteral("userAgent")] = userAgent;
    if (!htmlBridgeUrl.isEmpty())
        o[QStringLiteral("htmlBridgeUrl")] = htmlBridgeUrl;
    if (!scriptPath.isEmpty())
        o[QStringLiteral("scriptPath")] = scriptPath;

    const int existingIndex = indexOfUserSourceJson(name);
    if (existingIndex >= 0)
        m_userSourcesJson.replace(existingIndex, o);
    else
        m_userSourcesJson.append(o);
    saveUserSources();
}

QSharedPointer<ISource> SourceManager::byName(const QString &name) const
{
    for (const auto &s : m_sources) {
        if (s->name() == name)
            return s;
    }
    return {};
}

QString SourceManager::hiddenSourcesFilePath()
{
    return appDataFilePath(QStringLiteral("hidden_sources.json"));
}

void SourceManager::loadHiddenNames()
{
    QFile file(hiddenSourcesFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;
    for (const QJsonValue &v : doc.array())
        m_hiddenBuiltinNames.insert(v.toString());
}

void SourceManager::saveHiddenNames() const
{
    QFile file(hiddenSourcesFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "SourceManager: could not write" << file.fileName();
        return;
    }
    QJsonArray arr;
    for (const QString &name : m_hiddenBuiltinNames)
        arr.append(name);
    file.write(QJsonDocument(arr).toJson());
}

int SourceManager::indexOfUserSourceJson(const QString &name) const
{
    for (int i = 0; i < m_userSourcesJson.size(); ++i) {
        if (m_userSourcesJson.at(i).toObject().value(QStringLiteral("name")).toString() == name)
            return i;
    }
    return -1;
}

void SourceManager::removeSource(const QString &name)
{
    bool wasCustom = false;
    const int existingIndex = indexOfUserSourceJson(name);
    if (existingIndex >= 0) {
        m_userSourcesJson.removeAt(existingIndex);
        saveUserSources();
        wasCustom = true;
    }

    // Built-ins can't be deleted; hide them instead.
    if (!wasCustom) {
        m_hiddenBuiltinNames.insert(name);
        saveHiddenNames();
    }

    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources.at(i)->name() == name) {
            m_sources.removeAt(i);
            break;
        }
    }
}

void SourceManager::restoreDefaultSources()
{
    m_hiddenBuiltinNames.clear();
    saveHiddenNames();
    m_sources.clear();
    loadDefaults();
    loadUserSources();
}

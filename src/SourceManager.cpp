// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SourceManager.h"

#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDebug>

#include "sources/DanbooruSource.h"
#include "sources/GelbooruSource.h"
#include "sources/ShimmieSource.h"
#include "sources/GelbooruHtmlSource.h"
#include "SiteAccess.h"
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
    if (flavor == QStringLiteral("shimmie"))
        return QSharedPointer<ShimmieSource>::create(name, baseUrl);
    if (flavor == QStringLiteral("gelbooru-html"))
        return QSharedPointer<GelbooruHtmlSource>::create(name, baseUrl);
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
        QStringLiteral("gelbooru-html"),
        QStringLiteral("moebooru"),
        QStringLiteral("e621"),
        QStringLiteral("philomena"),
        QStringLiteral("szurubooru"),
        QStringLiteral("shimmie"),
    };
}

void SourceManager::loadDefaults(const QString &onlyName)
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
        if (!onlyName.isEmpty() && name != onlyName)
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
    publishSiteAccess();
}

void SourceManager::publishSiteAccess() const
{
    QHash<QString, SiteAccess::Credentials> byHost;
    for (const QJsonValue &v : m_userSourcesJson) {
        const QJsonObject o = v.toObject();
        SiteAccess::Credentials credentials;
        credentials.cookie = o.value(QStringLiteral("cookie")).toString();
        credentials.userAgent = o.value(QStringLiteral("userAgent")).toString();
        if (credentials.cookie.isEmpty() && credentials.userAgent.isEmpty())
            continue;
        byHost.insert(SiteAccess::hostOf(o.value(QStringLiteral("baseUrl")).toString()), credentials);
    }
    SiteAccess::setAll(byHost);
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

// QSaveFile: the old file stays intact until the new one is complete, so a
// crash mid-write cannot leave a truncated list that loses every source.
// ownerOnly: QSaveFile keeps the permissions of an existing target, so the
// target is created empty with 0600 first; nothing secret is in it yet.
static void writeJsonAtomically(const QString &path, const QJsonDocument &doc, bool ownerOnly)
{
    const QFileDevice::Permissions privatePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (ownerOnly && !QFile::exists(path)) {
        QFile create(path);
        if (create.open(QIODevice::WriteOnly))
            create.setPermissions(privatePermissions);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "SourceManager: could not write" << path;
        return;
    }
    file.write(doc.toJson());
    if (!file.commit()) {
        qWarning() << "SourceManager: could not write" << path << file.errorString();
        return;
    }
    // Also tightens a file written by an older version.
    if (ownerOnly)
        QFile::setPermissions(path, privatePermissions);
}

void SourceManager::saveUserSources() const
{
    // Holds API keys and session cookies.
    writeJsonAtomically(userSourcesFilePath(), QJsonDocument(m_userSourcesJson), /*ownerOnly=*/true);
    publishSiteAccess();
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
    QJsonArray arr;
    for (const QString &name : m_hiddenBuiltinNames)
        arr.append(name);
    writeJsonAtomically(hiddenSourcesFilePath(), QJsonDocument(arr), /*ownerOnly=*/false);
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

    // A custom source may have replaced a built-in of the same name. With the
    // custom one gone, the built-in comes back now, not only after a restart.
    if (wasCustom)
        loadDefaults(name);
}

void SourceManager::restoreDefaultSources()
{
    m_hiddenBuiltinNames.clear();
    saveHiddenNames();
    m_sources.clear();
    loadDefaults();
    loadUserSources();
}

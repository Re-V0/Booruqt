// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SiteAccess.h"

#include <QMutex>
#include <QMutexLocker>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QMutex g_mutex;
QHash<QString, SiteAccess::Credentials> g_byHost;

// "www.site.org" and "site.org" are one site to the user.
QString withoutWww(const QString &host)
{
    return host.startsWith(QStringLiteral("www.")) ? host.mid(4) : host;
}

} // namespace

namespace SiteAccess {

void setAll(const QHash<QString, Credentials> &byHost)
{
    const QMutexLocker locker(&g_mutex);
    g_byHost.clear();
    for (auto it = byHost.constBegin(); it != byHost.constEnd(); ++it) {
        if (!it.key().isEmpty() && (!it.value().cookie.isEmpty() || !it.value().userAgent.isEmpty()))
            g_byHost.insert(withoutWww(it.key().toLower()), it.value());
    }
}

QString hostOf(const QString &baseUrl)
{
    return QUrl(baseUrl).host().toLower();
}

void apply(QNetworkRequest &request)
{
    QString host = withoutWww(request.url().host().toLower());
    if (host.isEmpty())
        return;

    Credentials found;
    bool matched = false;
    {
        const QMutexLocker locker(&g_mutex);
        if (g_byHost.isEmpty())
            return;
        // The host itself, then each parent domain: a cookie set for
        // "site.org" also covers "img.site.org", the way browsers send it.
        while (!host.isEmpty()) {
            const auto it = g_byHost.constFind(host);
            if (it != g_byHost.constEnd()) {
                found = it.value();
                matched = true;
                break;
            }
            const int dot = host.indexOf(QLatin1Char('.'));
            if (dot < 0 || host.indexOf(QLatin1Char('.'), dot + 1) < 0)
                break; // never fall back to a bare top-level domain
            host = host.mid(dot + 1);
        }
    }
    if (!matched)
        return;

    if (!found.cookie.isEmpty())
        request.setRawHeader("Cookie", found.cookie.toUtf8());
    if (!found.userAgent.isEmpty())
        request.setHeader(QNetworkRequest::UserAgentHeader, found.userAgent);
}

} // namespace SiteAccess

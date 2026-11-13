// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SiteAccess.h"
#include "NetworkLog.h"

#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>
#include <memory>
#include <utility>

namespace {

QMutex g_mutex;
QHash<QString, SiteAccess::Credentials> g_byHost;

// "www.site.org" and "site.org" are one site to the user.
QString withoutWww(const QString &host)
{
    return host.startsWith(QStringLiteral("www.")) ? host.mid(4) : host;
}

const char kRefusedRedirectProperty[] = "blairqtRefusedRedirect";

// What is registered for a host: its own entry, else a parent domain's.
bool lookup(QString host, SiteAccess::Credentials *found)
{
    const QMutexLocker locker(&g_mutex);
    if (g_byHost.isEmpty())
        return false;
    // The host itself, then each parent domain: a cookie set for
    // "site.org" also covers "img.site.org", the way browsers send it.
    while (!host.isEmpty()) {
        const auto it = g_byHost.constFind(host);
        if (it != g_byHost.constEnd()) {
            *found = it.value();
            return true;
        }
        const int dot = host.indexOf(QLatin1Char('.'));
        if (dot < 0 || host.indexOf(QLatin1Char('.'), dot + 1) < 0)
            return false; // never fall back to a bare top-level domain
        host = host.mid(dot + 1);
    }
    return false;
}

// Whether a request that went to `from` with `cookie` may be sent on to `to`.
bool redirectKeepsCookieOnSite(const QUrl &from, const QUrl &to, const QByteArray &cookie)
{
    if (from.scheme() == QStringLiteral("https") && to.scheme() != QStringLiteral("https"))
        return false;
    const QString fromHost = withoutWww(from.host().toLower());
    const QString toHost = withoutWww(to.host().toLower());
    if (fromHost == toHost)
        return true;
    SiteAccess::Credentials registered;
    return lookup(toHost, &registered) && !registered.cookie.isEmpty()
        && registered.cookie.toUtf8() == cookie;
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
    const QString host = withoutWww(request.url().host().toLower());
    if (host.isEmpty())
        return;

    Credentials found;
    if (!lookup(host, &found))
        return;

    if (!found.cookie.isEmpty())
        request.setRawHeader("Cookie", found.cookie.toUtf8());
    if (!found.userAgent.isEmpty())
        request.setHeader(QNetworkRequest::UserAgentHeader, found.userAgent);
}

QNetworkReply *get(QNetworkAccessManager *manager, QNetworkRequest request)
{
    apply(request);
    return getPrepared(manager, std::move(request));
}

QNetworkReply *getPrepared(QNetworkAccessManager *manager, QNetworkRequest request)
{
    const QByteArray cookie = request.rawHeader("Cookie");
    if (cookie.isEmpty())
        return manager->get(request);

    // Qt resends a request's headers to wherever a redirect points, so each
    // redirect of a request with a cookie is checked before it is followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    QNetworkReply *reply = manager->get(request);
    // The address this hop went to; a chain of redirects moves it along.
    auto current = std::make_shared<QUrl>(request.url());
    QObject::connect(reply, &QNetworkReply::redirected, reply, [reply, cookie, current](const QUrl &target) {
        if (redirectKeepsCookieOnSite(*current, target, cookie)) {
            *current = target;
            emit reply->redirectAllowed();
            return;
        }
        NetworkLog::instance().log(QStringLiteral("  -> REFUSED redirect from %1 to %2: the site's cookie would go with it")
            .arg(current->host(), target.host()));
        reply->setProperty(kRefusedRedirectProperty, target.toString());
        reply->abort();
    });
    return reply;
}

QString refusedRedirect(const QNetworkReply *reply)
{
    return reply->property(kRefusedRedirectProperty).toString();
}

} // namespace SiteAccess

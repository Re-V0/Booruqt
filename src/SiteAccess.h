// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QHash>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// The cookie and User-Agent a user pasted for a site behind bot protection
// (a Cloudflare cf_clearance, say). Such a cookie is only accepted together
// with the browser's User-Agent, and it is needed by every request to the
// site - searches, tag suggestions, thumbnails, downloads, the viewer -
// whatever engine the site runs. So they are kept per host here, and every
// place that sends a request calls apply().
//
// A cookie belongs to its site, and Qt would carry a request's headers along
// when the server redirects it elsewhere. So requests are sent through get(),
// which lets a request with a cookie follow a redirect only within the site.
//
// Thread-safe: plugin sources build requests on worker threads.
namespace SiteAccess {

struct Credentials {
    QString cookie;
    QString userAgent;
};

// Replaces everything known with this: host -> what to send there.
void setAll(const QHash<QString, Credentials> &byHost);

// The host part of a source's base address, as setAll() keys it.
QString hostOf(const QString &baseUrl);

// Adds the cookie and User-Agent registered for the request's host, or for
// a domain it is a subdomain of. Leaves other requests alone.
void apply(QNetworkRequest &request);

// apply(), then a GET. A request that carries a cookie - from here, from its
// source or from a plugin - follows a redirect only to the same site (the
// same host, or one this cookie is registered for) and never from https to
// http. Any other redirect ends the request with an error, and
// refusedRedirect() names where it pointed.
QNetworkReply *get(QNetworkAccessManager *manager, QNetworkRequest request);

// The same without apply(), for a request whose headers are already final
// (the "add a booru" dialog sends the cookie typed into it).
QNetworkReply *getPrepared(QNetworkAccessManager *manager, QNetworkRequest request);

// The address a reply from get() was redirected to and did not follow;
// empty for every other reply.
QString refusedRedirect(const QNetworkReply *reply);

} // namespace SiteAccess

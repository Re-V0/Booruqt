// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "NetworkLog.h"

#include <QTime>
#include <QUrlQuery>
#include <QStringList>

NetworkLog &NetworkLog::instance()
{
    static NetworkLog inst;
    return inst;
}

void NetworkLog::log(const QString &line)
{
    const QString stamped = QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + line;
    emit entryAdded(stamped);
}

QString NetworkLog::redactedUrl(const QUrl &url)
{
    static const QStringList secretKeys = {
        QStringLiteral("api_key"), QStringLiteral("user_id"),
        QStringLiteral("login"), QStringLiteral("password_hash"),
    };

    QUrlQuery query(url);
    bool changed = false;
    for (const QString &key : secretKeys) {
        if (!query.hasQueryItem(key))
            continue;
        query.removeAllQueryItems(key);
        query.addQueryItem(key, QStringLiteral("***"));
        changed = true;
    }
    if (!changed)
        return url.toString();

    QUrl copy = url;
    copy.setQuery(query);
    return copy.toString();
}

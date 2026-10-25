// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

// Process-wide log of outgoing requests and responses, for debugging.
class NetworkLog : public QObject {
    Q_OBJECT
public:
    static NetworkLog &instance();

    void log(const QString &line);

    // The URL as text with credentials in the query (api_key, user_id, ...)
    // replaced by "***". The log is on screen, so it must never show them.
    static QString redactedUrl(const QUrl &url);

signals:
    void entryAdded(const QString &stampedLine);

private:
    NetworkLog() = default;
};

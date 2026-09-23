// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ISource.h"
#include <QJsonDocument>
#include <QJsonObject>

// "python" flavor: runs a user script as a subprocess. Contract:
//   def build_search_request(base_url, tags, page, per_page) -> dict
//   def parse_response(body, base_url) -> list[dict]
//   def supports_tag_exclusion() -> bool   (optional)
class PythonPluginSource : public ISource {
public:
    PythonPluginSource(QString name, QString baseUrl, QString scriptPath);

    QString name() const override { return m_name; }
    QString flavor() const override { return QStringLiteral("python"); }
    QString baseUrl() const override { return m_baseUrl; }
    QString scriptPath() const { return m_scriptPath; }

    QNetworkRequest buildSearchRequest(const QString &tags, int page, int perPage) const override;
    QVector<Post> parseSearchResponse(const QByteArray &data) const override;

    // Asks the script once and remembers the answer; spawning python per
    // search just to re-ask would be wasteful.
    bool supportsTagExclusion() const override;

    // Every call shells out to python3.
    bool mayBlock() const override { return true; }

private:
    // Runs the script via plugin_runner.py; null QJsonDocument on failure.
    QJsonDocument runPlugin(const QString &mode, const QJsonObject &stdinJson) const;
    static QString runnerScriptPath();

    QString m_name;
    QString m_baseUrl;
    QString m_scriptPath;
    // -1 not asked yet, 0 no, 1 yes.
    mutable int m_exclusionSupport = -1;
};

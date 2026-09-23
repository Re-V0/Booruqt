// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "PythonPluginSource.h"
#include "../NetworkLog.h"

#include <QProcess>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QUrl>
#include <utility>

PythonPluginSource::PythonPluginSource(QString name, QString baseUrl, QString scriptPath)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl)), m_scriptPath(std::move(scriptPath))
{
}

QString PythonPluginSource::runnerScriptPath()
{
    // Same dev-vs-installed lookup as main.cpp's loadTranslation().
    const QString devPath = QCoreApplication::applicationDirPath() + QStringLiteral("/plugin_runner.py");
    if (QFile::exists(devPath))
        return devPath;

#ifdef BLAIRQT_PLUGIN_RUNNER_PATH
    const QString installedPath = QStringLiteral(BLAIRQT_PLUGIN_RUNNER_PATH);
    if (QFile::exists(installedPath))
        return installedPath;
#endif

    return devPath;
}

QJsonDocument PythonPluginSource::runPlugin(const QString &mode, const QJsonObject &stdinJson) const
{
    if (!QFile::exists(m_scriptPath)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: script not found: %2")
            .arg(m_name, m_scriptPath));
        return {};
    }

    QProcess process;
    process.start(QStringLiteral("python3"), {runnerScriptPath(), m_scriptPath, mode});
    if (!process.waitForStarted(3000)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: could not start python3 (is it installed?)")
            .arg(m_name));
        return {};
    }

    process.write(QJsonDocument(stdinJson).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();

    if (!process.waitForFinished(10000)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: %2 timed out after 10s")
            .arg(m_name, mode));
        process.kill();
        process.waitForFinished();
        return {};
    }

    if (process.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1] in %2:\n%3")
            .arg(m_name, mode, stderrText.isEmpty() ? QStringLiteral("(no output)") : stderrText));
        return {};
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: %2 did not print valid JSON (%3)")
            .arg(m_name, mode, parseError.errorString()));
        return {};
    }
    return doc;
}

bool PythonPluginSource::supportsTagExclusion() const
{
    if (m_exclusionSupport < 0) {
        const QJsonObject caps = runPlugin(QStringLiteral("capabilities"), QJsonObject()).object();
        m_exclusionSupport = caps.value(QStringLiteral("supports_tag_exclusion")).toBool() ? 1 : 0;
    }
    return m_exclusionSupport == 1;
}

QNetworkRequest PythonPluginSource::buildSearchRequest(const QString &tags, int page, int perPage) const
{
    QJsonObject params;
    params[QStringLiteral("base_url")] = m_baseUrl;
    params[QStringLiteral("tags")] = tags;
    params[QStringLiteral("page")] = page;
    params[QStringLiteral("per_page")] = perPage;

    const QJsonObject result = runPlugin(QStringLiteral("build_request"), params).object();
    const QString url = result.value(QStringLiteral("url")).toString();
    if (url.isEmpty())
        return QNetworkRequest(QUrl()); // invalid - fails fast, visible in the network log

    QNetworkRequest request{QUrl(url)};
    const QJsonObject headers = result.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    return request;
}

QVector<Post> PythonPluginSource::parseSearchResponse(const QByteArray &data) const
{
    QJsonObject params;
    params[QStringLiteral("body_base64")] = QString::fromLatin1(data.toBase64());
    params[QStringLiteral("base_url")] = m_baseUrl;

    const QJsonDocument result = runPlugin(QStringLiteral("parse_response"), params);

    QVector<Post> posts;
    for (const QJsonValue &v : result.array()) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const QString fileUrl = o.value(QStringLiteral("file_url")).toString();
        if (id.isEmpty() || fileUrl.isEmpty())
            continue;

        Post post;
        post.id = id;
        post.sourceName = m_name;
        post.fileUrl = fileUrl;
        post.previewUrl = o.value(QStringLiteral("preview_url")).toString(fileUrl);
        post.sampleUrl = o.value(QStringLiteral("sample_url")).toString(post.previewUrl);
        post.postPageUrl = o.value(QStringLiteral("post_page_url")).toString();
        post.rating = o.value(QStringLiteral("rating")).toString();
        post.score = o.value(QStringLiteral("score")).toInt();
        post.width = o.value(QStringLiteral("width")).toInt();
        post.height = o.value(QStringLiteral("height")).toInt();
        for (const QJsonValue &tag : o.value(QStringLiteral("tags")).toArray())
            post.tags.append(tag.toString());
        posts.append(post);
    }
    return posts;
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "PythonPluginSource.h"
#include "../NetworkLog.h"
#include "../AppInfo.h"

#include <QProcess>
#include <QFile>
#include <QJsonArray>
#include <QUrl>
#include <QStandardPaths>
#include <utility>

namespace {

// The interpreter and the arguments that select Python 3, or an empty list.
// BLAIRQT_PYTHON overrides the search, for an interpreter not on PATH.
//
// Windows: "py -3" first. It is the launcher python.org installs, while
// "python3" and "python" on a machine without Python are Microsoft Store
// stubs that open the Store instead of running anything.
QStringList pythonCommand()
{
    const QString overridden = qEnvironmentVariable("BLAIRQT_PYTHON");
    if (!overridden.isEmpty())
        return {overridden};

#ifdef Q_OS_WIN
    const QString launcher = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!launcher.isEmpty())
        return {launcher, QStringLiteral("-3")};
    const QStringList names = {QStringLiteral("python"), QStringLiteral("python3")};
#else
    const QStringList names = {QStringLiteral("python3"), QStringLiteral("python")};
#endif
    for (const QString &name : names) {
        const QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty())
            return {path};
    }
    return {};
}

} // namespace

PythonPluginSource::PythonPluginSource(QString name, QString baseUrl, QString scriptPath)
    : m_name(std::move(name)), m_baseUrl(std::move(baseUrl)), m_scriptPath(std::move(scriptPath))
{
}

QString PythonPluginSource::runnerScriptPath()
{
    return findAppFile(QStringLiteral("plugin_runner.py"));
}

QJsonDocument PythonPluginSource::runPlugin(const QString &mode, const QJsonObject &stdinJson) const
{
    if (!QFile::exists(m_scriptPath)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: script not found: %2")
            .arg(m_name, m_scriptPath));
        return {};
    }

    const QStringList python = pythonCommand();
    if (python.isEmpty()) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: no Python 3 found "
                                                  "(looked for py, python3, python; or set BLAIRQT_PYTHON)")
            .arg(m_name));
        return {};
    }

    QStringList arguments = python.mid(1);
    arguments << runnerScriptPath() << m_scriptPath << mode;

    QProcess process;
    process.start(python.first(), arguments);
    if (!process.waitForStarted(3000)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: could not start %2")
            .arg(m_name, python.first()));
        return {};
    }

    process.write(QJsonDocument(stdinJson).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();

    // build_request may talk to the network itself (Newgrounds solves a
    // proof-of-work challenge there), so it gets far longer than parsing,
    // which only reads the body it is given. All of this runs off the GUI
    // thread, so a long wait freezes nothing.
    const int timeoutMs = mode == QStringLiteral("build_request") ? 60000 : 10000;
    if (!process.waitForFinished(timeoutMs)) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN ERROR [%1]: %2 timed out after %3s")
            .arg(m_name, mode).arg(timeoutMs / 1000));
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
    int support = m_exclusionSupport.load();
    if (support < 0) {
        const QJsonObject caps = runPlugin(QStringLiteral("capabilities"), QJsonObject()).object();
        support = caps.value(QStringLiteral("supports_tag_exclusion")).toBool() ? 1 : 0;
        m_exclusionSupport.store(support);
    }
    return support == 1;
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
    int skipped = 0;
    for (const QJsonValue &v : result.array()) {
        const QJsonObject o = v.toObject();
        // A Python plugin naturally returns "id": 42; toString() alone would
        // turn that into "" and drop the post.
        const QJsonValue idValue = o.value(QStringLiteral("id"));
        QString id = idValue.toString();
        if (id.isEmpty() && idValue.isDouble())
            id = QString::number(idValue.toInteger());
        const QString fileUrl = o.value(QStringLiteral("file_url")).toString();
        if (id.isEmpty() || fileUrl.isEmpty()) {
            ++skipped;
            continue;
        }

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

    if (skipped > 0) {
        NetworkLog::instance().log(QStringLiteral("  -> PLUGIN [%1]: skipped %2 post(s) without \"id\" or \"file_url\"")
            .arg(m_name).arg(skipped));
    }
    return posts;
}

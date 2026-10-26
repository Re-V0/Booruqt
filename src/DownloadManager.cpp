// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DownloadManager.h"
#include "NetworkLog.h"
#include "LibraryDatabase.h"
#include "FolderAccessGuard.h"
#include "AppInfo.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>
#include <QSaveFile>

#ifdef Q_OS_WIN
#define NOMINMAX // windows.h would otherwise define min()/max() macros
#include <windows.h>
#endif

namespace {

// Writes the tags sidecar. Unix hides it by the leading dot in its name;
// Windows ignores the dot and needs the hidden attribute, without which
// every image would get a visible .txt neighbour in Explorer.
void writeSidecar(const QString &path, const QStringList &tags)
{
#ifdef Q_OS_WIN
    // Windows refuses to truncate an existing hidden file for writing.
    QFile::remove(path);
#endif
    QFile sidecar(path);
    if (!sidecar.open(QIODevice::WriteOnly))
        return;
    sidecar.write(tags.join(QLatin1Char(' ')).toUtf8());
    sidecar.close();
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    SetFileAttributesW(reinterpret_cast<const wchar_t *>(nativePath.utf16()), FILE_ATTRIBUTE_HIDDEN);
#endif
}

} // namespace

DownloadManager::DownloadManager(LibraryDatabase *library, QObject *parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this)), m_library(library)
{
}

void DownloadManager::setDownloadDir(const QString &dir)
{
    m_downloadDir = dir;
    // Setting it is itself the explicit-consent action; auto-grant.
    FolderAccessGuard::addAllowedFolder(dir);
}

QString DownloadManager::sidecarPathFor(const QString &imagePath)
{
    const QFileInfo fi(imagePath);
    return fi.absolutePath() + QLatin1Char('/') + QLatin1Char('.') + fi.fileName() + QStringLiteral(".txt");
}

QString DownloadManager::targetFilePath(const Post &post) const
{
    // The extension, source and id all come from the site or a plugin, and
    // a "/" in any of them would point the file into a folder that does not
    // exist.
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]"));

    QString ext = QFileInfo(QUrl(post.fileUrl).path()).suffix();
    if (ext.isEmpty() || ext.size() > 5)
        ext = post.fileExtension;
    if (ext.isEmpty() || ext.size() > 5)
        ext = QStringLiteral("jpg");
    ext.replace(unsafe, QStringLiteral("_"));
    const QString safeSource = QString(post.sourceName).replace(unsafe, QStringLiteral("_"));
    const QString safeId = QString(post.id).replace(unsafe, QStringLiteral("_"));
    const QString fileName = QStringLiteral("%1_%2.%3").arg(safeSource, safeId, ext);
    return m_downloadDir + QLatin1Char('/') + fileName;
}

void DownloadManager::download(const Post &post)
{
    if (post.fileUrl.isEmpty()) {
        emit downloadFinished(QString(), false, false, tr("post nie ma pełnego pliku (fileUrl puste)"));
        return;
    }

    // Already downloaded, possibly moved into a subfolder since.
    if (m_library) {
        const QString knownPath = m_library->pathForPost(post.sourceName, post.id);
        if (!knownPath.isEmpty() && QFile::exists(knownPath)) {
            m_library->addOrUpdatePost(post, knownPath);
            emit downloadFinished(knownPath, true, true, QString());
            return;
        }
    }

    const QString path = targetFilePath(post);
    if (!FolderAccessGuard::isAllowed(path)) {
        NetworkLog::instance().log(QStringLiteral("  -> REFUSED: %1 is outside the allowed folders").arg(path));
        emit downloadFinished(QString(), false, false,
            tr("folder pobierania jest poza dozwolonym zakresem dostępu"));
        return;
    }

    QDir().mkpath(m_downloadDir);

    if (QFile::exists(path)) {
        // Already on disk - re-index it in case the DB predates it.
        if (m_library)
            m_library->addOrUpdatePost(post, path);
        emit downloadFinished(path, true, true, QString());
        return;
    }

    // A second click while the first download still runs: both would write
    // the same file.
    if (m_inFlight.contains(path)) {
        NetworkLog::instance().log(QStringLiteral("  -> already downloading %1").arg(path));
        return;
    }

    // QSaveFile writes to a temporary file next to the target and renames it
    // only on commit(). A crash or a broken connection therefore leaves no
    // half file behind that a later QFile::exists() would take as done.
    auto *file = new QSaveFile(path);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        emit downloadFinished(QString(), false, false, tr("nie można zapisać %1").arg(path));
        return;
    }

    NetworkLog::instance().log(QStringLiteral("GET %1 (download)").arg(post.fileUrl));

    QNetworkRequest req{QUrl(post.fileUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    req.setTransferTimeout(30000);

    QNetworkReply *reply = m_manager->get(req);
    file->setParent(reply); // deleted together with the reply
    m_inFlight.insert(path);

    // Written as it arrives, so a large video is never held in memory whole.
    connect(reply, &QNetworkReply::readyRead, reply, [reply, file]() {
        const QByteArray chunk = reply->readAll();
        if (file->write(chunk) != chunk.size()) {
            // Disk full or similar: stop instead of downloading into nothing.
            file->cancelWriting();
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, file, path, post]() {
        reply->deleteLater();
        m_inFlight.remove(path);

        if (file->error() != QFileDevice::NoError) {
            NetworkLog::instance().log(QStringLiteral("  -> ERROR writing %1: %2").arg(path, file->errorString()));
            emit downloadFinished(QString(), false, false, tr("nie można zapisać %1").arg(path));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            file->cancelWriting();
            NetworkLog::instance().log(QStringLiteral("  -> ERROR downloading: %1").arg(reply->errorString()));
            emit downloadFinished(QString(), false, false, reply->errorString());
            return;
        }

        // A Cloudflare challenge or a login page arrives as 200 OK too.
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (contentType.startsWith(QStringLiteral("text/html"))) {
            file->cancelWriting();
            NetworkLog::instance().log(QStringLiteral("  -> REFUSED: %1 answered with a web page (%2)")
                .arg(post.fileUrl, contentType));
            emit downloadFinished(QString(), false, false,
                tr("serwer zwrócił stronę WWW zamiast pliku (Cloudflare albo wymagane logowanie?)"));
            return;
        }

        // Normally readyRead has taken everything; this catches the rest.
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty() && file->write(rest) != rest.size())
            file->cancelWriting();
        if (!file->commit()) {
            NetworkLog::instance().log(QStringLiteral("  -> ERROR writing %1: %2").arg(path, file->errorString()));
            emit downloadFinished(QString(), false, false, tr("nie można zapisać %1").arg(path));
            return;
        }

        if (!post.tags.isEmpty())
            writeSidecar(sidecarPathFor(path), post.tags);

        if (m_library)
            m_library->addOrUpdatePost(post, path);

        NetworkLog::instance().log(QStringLiteral("  -> saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        emit downloadFinished(path, true, false, QString());
    });
}

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
    QString ext = QFileInfo(QUrl(post.fileUrl).path()).suffix();
    if (ext.isEmpty() || ext.size() > 5)
        ext = QStringLiteral("jpg");

    const QString safeSource = QString(post.sourceName).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    const QString fileName = QStringLiteral("%1_%2.%3").arg(safeSource, post.id, ext);
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

    NetworkLog::instance().log(QStringLiteral("GET %1 (download)").arg(post.fileUrl));

    QNetworkRequest req{QUrl(post.fileUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
    req.setTransferTimeout(30000);

    QNetworkReply *reply = m_manager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, path, post]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            NetworkLog::instance().log(QStringLiteral("  -> ERROR downloading: %1").arg(reply->errorString()));
            emit downloadFinished(QString(), false, false, reply->errorString());
            return;
        }

        const QByteArray data = reply->readAll();
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            emit downloadFinished(QString(), false, false, tr("nie można zapisać %1").arg(path));
            return;
        }
        // A short write (e.g. full disk) must not be indexed as a good file.
        if (file.write(data) != data.size()) {
            file.close();
            QFile::remove(path);
            emit downloadFinished(QString(), false, false, tr("nie można zapisać %1").arg(path));
            return;
        }
        file.close();

        if (!post.tags.isEmpty()) {
            QFile sidecar(sidecarPathFor(path));
            if (sidecar.open(QIODevice::WriteOnly))
                sidecar.write(post.tags.join(QLatin1Char(' ')).toUtf8());
        }

        if (m_library)
            m_library->addOrUpdatePost(post, path);

        NetworkLog::instance().log(QStringLiteral("  -> saved %1 (%2 bytes)").arg(path).arg(data.size()));
        emit downloadFinished(path, true, false, QString());
    });
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "LibraryImport.h"
#include "DownloadManager.h"
#include "FolderAccessGuard.h"
#include "LibraryDatabase.h"
#include "Post.h"

#include <QByteArrayList>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("LibraryImport", text);
}

// Files to import, folders walked; each file once, in the order given.
QStringList collectFiles(const QStringList &paths)
{
    QStringList files;
    QSet<QString> seen;
    const auto add = [&files, &seen](const QString &path) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!seen.contains(absolute)) {
            seen.insert(absolute);
            files.append(absolute);
        }
    };
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isDir()) {
            add(path);
            continue;
        }
        QStringList inside;
        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            inside.append(it.next());
        inside.sort();
        for (const QString &file : std::as_const(inside))
            add(file);
    }
    return files;
}

// The file's MD5: the same picture twice is one entry, whatever its name.
QString contentHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

QStringList sidecarTags(const QString &imagePath)
{
    QFile sidecar(DownloadManager::sidecarPathFor(imagePath));
    if (!sidecar.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(sidecar.readAll()).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// "name.png", else "name (2).png", "name (3).png"...
QString freeName(const QDir &dir, const QString &fileName)
{
    if (!dir.exists(fileName))
        return dir.filePath(fileName);
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!dir.exists(candidate))
            return dir.filePath(candidate);
    }
}

bool isInside(const QString &path, const QString &root)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    return !canonicalRoot.isEmpty() && canonicalPath.startsWith(canonicalRoot + QLatin1Char('/'));
}

} // namespace

namespace LibraryImport {

bool isMedia(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return false;
    if (isVideoExtension(suffix))
        return true;
    static const QByteArrayList formats = QImageReader::supportedImageFormats();
    return formats.contains(suffix.toLatin1());
}

Result importPaths(LibraryDatabase *library, const QStringList &paths, const QString &targetDir,
                   const QString &libraryRoot, const QStringList &tags)
{
    Result result;
    if (!library || !library->isOpen()) {
        result.errors.append(tr("baza biblioteki niedostępna"));
        return result;
    }
    // The folder must exist for the guard to place it, and only an allowed
    // one is written into.
    QDir().mkpath(targetDir);
    if (!FolderAccessGuard::isAllowed(targetDir)) {
        result.errors.append(tr("%1 jest poza dozwolonym zakresem dostępu").arg(targetDir));
        return result;
    }
    const QDir target(targetDir);

    for (const QString &source : collectFiles(paths)) {
        const QString fileName = QFileInfo(source).fileName();
        // Hidden files are tag sidecars and the like, not pictures.
        if (fileName.startsWith(QLatin1Char('.')))
            continue;
        if (!isMedia(source)) {
            ++result.notMedia;
            continue;
        }
        const QString id = contentHash(source);
        if (id.isEmpty()) {
            ++result.failed;
            result.errors.append(tr("%1: nie można odczytać pliku").arg(source));
            continue;
        }
        const QString known = library->pathForPost(kSourceName, id);
        if (!known.isEmpty() && QFile::exists(known)) {
            ++result.alreadyInLibrary;
            continue;
        }
        // A file of the library's folders that is indexed already - a
        // download, say - keeps its entry, its site and its tags.
        const bool inLibraryFolders = isInside(source, libraryRoot);
        if (inLibraryFolders && library->hasFile(source)) {
            ++result.alreadyInLibrary;
            continue;
        }

        QStringList fileTags = sidecarTags(source);
        for (const QString &tag : tags) {
            if (!fileTags.contains(tag))
                fileTags.append(tag);
        }

        // Already in the library's folders: indexed where it lies.
        QString destination = source;
        if (!inLibraryFolders) {
            destination = freeName(target, fileName);
            if (!QFile::copy(source, destination)) {
                ++result.failed;
                result.errors.append(tr("%1: nie można skopiować do %2").arg(source, targetDir));
                continue;
            }
        }
        if (!fileTags.isEmpty())
            DownloadManager::writeSidecar(DownloadManager::sidecarPathFor(destination), fileTags);

        Post post;
        post.id = id;
        post.sourceName = kSourceName;
        post.tags = fileTags;
        const QSize size = QImageReader(destination).size();
        post.width = size.isValid() ? size.width() : 0;
        post.height = size.isValid() ? size.height() : 0;
        library->addOrUpdatePost(post, destination);
        ++result.imported;
        result.importedPaths.append(destination);
    }
    return result;
}

} // namespace LibraryImport

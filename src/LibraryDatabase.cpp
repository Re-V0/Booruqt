// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "LibraryDatabase.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QStringList>
#include <algorithm>
#include <QUrl>

LibraryDatabase::LibraryDatabase()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);

    // A unique connection name (rather than the default "qt_sql_default_connection")
    // so this doesn't collide if the app ever opens a second QSqlDatabase elsewhere.
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("blairqt_library"));
    m_db.setDatabaseName(dir + QStringLiteral("/library.sqlite3"));

    if (!m_db.open()) {
        qWarning() << "LibraryDatabase: failed to open" << m_db.databaseName() << m_db.lastError().text();
        return;
    }

    ensureSchema();
}

LibraryDatabase::~LibraryDatabase()
{
    if (m_db.isOpen())
        m_db.close();
}

bool LibraryDatabase::isOpen() const
{
    return m_db.isOpen();
}

void LibraryDatabase::ensureSchema()
{
    QSqlQuery q(m_db);

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  source TEXT NOT NULL,"
        "  post_id TEXT NOT NULL,"
        "  file_path TEXT NOT NULL UNIQUE,"
        "  file_url TEXT,"
        "  preview_url TEXT,"
        "  rating TEXT,"
        "  score INTEGER,"
        "  width INTEGER,"
        "  height INTEGER,"
        "  source_url TEXT,"
        "  post_page_url TEXT,"
        "  tags TEXT,"
        "  downloaded_at TEXT NOT NULL"
        ")"));

    // External-content FTS5: no duplicated tag storage, synced via triggers.
    q.exec(QStringLiteral(
        "CREATE VIRTUAL TABLE IF NOT EXISTS posts_fts USING fts5("
        "  tags, content='posts', content_rowid='id'"
        ")"));

    q.exec(QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS posts_ai AFTER INSERT ON posts BEGIN"
        "  INSERT INTO posts_fts(rowid, tags) VALUES (new.id, new.tags);"
        "END"));
    q.exec(QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS posts_ad AFTER DELETE ON posts BEGIN"
        "  INSERT INTO posts_fts(posts_fts, rowid, tags) VALUES('delete', old.id, old.tags);"
        "END"));
    q.exec(QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS posts_au AFTER UPDATE ON posts BEGIN"
        "  INSERT INTO posts_fts(posts_fts, rowid, tags) VALUES('delete', old.id, old.tags);"
        "  INSERT INTO posts_fts(rowid, tags) VALUES (new.id, new.tags);"
        "END"));
}

void LibraryDatabase::addOrUpdatePost(const Post &post, const QString &filePath)
{
    if (!m_db.isOpen())
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO posts (source, post_id, file_path, file_url, preview_url, rating, score, "
        "                   width, height, source_url, post_page_url, tags, downloaded_at) "
        "VALUES (:source, :post_id, :file_path, :file_url, :preview_url, :rating, :score, "
        "        :width, :height, :source_url, :post_page_url, :tags, :downloaded_at) "
        "ON CONFLICT(file_path) DO UPDATE SET "
        "  source=excluded.source, post_id=excluded.post_id, file_url=excluded.file_url, "
        "  preview_url=excluded.preview_url, rating=excluded.rating, score=excluded.score, "
        "  width=excluded.width, height=excluded.height, source_url=excluded.source_url, "
        "  post_page_url=excluded.post_page_url, tags=excluded.tags, downloaded_at=excluded.downloaded_at"));

    q.bindValue(QStringLiteral(":source"), post.sourceName);
    q.bindValue(QStringLiteral(":post_id"), post.id);
    q.bindValue(QStringLiteral(":file_path"), filePath);
    q.bindValue(QStringLiteral(":file_url"), post.fileUrl);
    q.bindValue(QStringLiteral(":preview_url"), post.previewUrl);
    q.bindValue(QStringLiteral(":rating"), post.rating);
    q.bindValue(QStringLiteral(":score"), post.score);
    q.bindValue(QStringLiteral(":width"), post.width);
    q.bindValue(QStringLiteral(":height"), post.height);
    q.bindValue(QStringLiteral(":source_url"), post.artistSourceUrl);
    q.bindValue(QStringLiteral(":post_page_url"), post.postPageUrl);
    q.bindValue(QStringLiteral(":tags"), post.tags.join(QLatin1Char(' ')));
    q.bindValue(QStringLiteral(":downloaded_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    if (!q.exec())
        qWarning() << "LibraryDatabase: insert failed:" << q.lastError().text();
}

static Post postFromQuery(const QSqlQuery &q)
{
    Post p;
    p.libraryId = q.value(QStringLiteral("id")).toLongLong();
    p.sourceName = q.value(QStringLiteral("source")).toString();
    p.id = q.value(QStringLiteral("post_id")).toString();
    p.localFilePath = q.value(QStringLiteral("file_path")).toString();
    p.fileUrl = q.value(QStringLiteral("file_url")).toString();
    p.previewUrl = QUrl::fromLocalFile(p.localFilePath).toString();
    p.rating = q.value(QStringLiteral("rating")).toString();
    p.score = q.value(QStringLiteral("score")).toInt();
    p.width = q.value(QStringLiteral("width")).toInt();
    p.height = q.value(QStringLiteral("height")).toInt();
    p.artistSourceUrl = q.value(QStringLiteral("source_url")).toString();
    p.postPageUrl = q.value(QStringLiteral("post_page_url")).toString();
    p.tags = q.value(QStringLiteral("tags")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return p;
}

namespace {

// Quotes each term so tags like "foo_(bar)" or "rating:safe" aren't FTS5 syntax.
QString ftsQueryFromTags(const QString &tags)
{
    QStringList terms;
    for (QString term : tags.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        term.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        terms.append(QLatin1Char('"') + term + QLatin1Char('"'));
    }
    return terms.join(QLatin1Char(' '));
}

QString escapeLike(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return value;
}

} // namespace

QVector<Post> LibraryDatabase::search(const QString &tagQuery, int limit, qint64 beforeId,
                                      const QString &dirFilter, bool includeSubdirs) const
{
    QVector<Post> results;
    if (!m_db.isOpen())
        return results;

    const QString ftsQuery = ftsQueryFromTags(tagQuery.trimmed());

    QStringList where;
    if (beforeId > 0)
        where << QStringLiteral("posts.id < :before_id");
    if (!ftsQuery.isEmpty())
        where << QStringLiteral("posts.id IN (SELECT rowid FROM posts_fts WHERE posts_fts MATCH :query)");
    if (!dirFilter.isEmpty()) {
        where << QStringLiteral("posts.file_path LIKE :dir ESCAPE '\\'");
        if (!includeSubdirs)
            where << QStringLiteral("posts.file_path NOT LIKE :deeper ESCAPE '\\'");
    }

    QString sql = QStringLiteral("SELECT posts.* FROM posts");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY posts.id DESC LIMIT :limit");

    QSqlQuery q(m_db);
    q.prepare(sql);
    if (beforeId > 0)
        q.bindValue(QStringLiteral(":before_id"), beforeId);
    if (!ftsQuery.isEmpty())
        q.bindValue(QStringLiteral(":query"), ftsQuery);
    if (!dirFilter.isEmpty()) {
        const QString prefix = escapeLike(QDir::cleanPath(dirFilter)) + QLatin1Char('/');
        q.bindValue(QStringLiteral(":dir"), prefix + QLatin1Char('%'));
        if (!includeSubdirs)
            q.bindValue(QStringLiteral(":deeper"), prefix + QStringLiteral("%/%"));
    }
    q.bindValue(QStringLiteral(":limit"), limit);

    if (!q.exec()) {
        qWarning() << "LibraryDatabase: search failed:" << q.lastError().text();
        return results;
    }

    while (q.next())
        results.append(postFromQuery(q));
    return results;
}

QString LibraryDatabase::pathForPost(const QString &source, const QString &postId) const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT file_path FROM posts WHERE source = :source AND post_id = :post_id LIMIT 1"));
    q.bindValue(QStringLiteral(":source"), source);
    q.bindValue(QStringLiteral(":post_id"), postId);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

QStringList LibraryDatabase::suggestTags(const QString &prefix, int limit) const
{
    if (!m_db.isOpen() || prefix.isEmpty())
        return {};

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tags FROM posts WHERE tags LIKE :needle ESCAPE '\\' ORDER BY id DESC LIMIT 5000"));
    q.bindValue(QStringLiteral(":needle"), QStringLiteral("%") + escapeLike(prefix) + QStringLiteral("%"));
    if (!q.exec())
        return {};

    QHash<QString, int> counts;
    while (q.next()) {
        for (const QString &tag : q.value(0).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (tag.startsWith(prefix, Qt::CaseInsensitive))
                ++counts[tag];
        }
    }

    QStringList tags = counts.keys();
    std::sort(tags.begin(), tags.end(), [&counts](const QString &a, const QString &b) {
        return counts.value(a) != counts.value(b) ? counts.value(a) > counts.value(b) : a < b;
    });
    return tags.mid(0, limit);
}

int LibraryDatabase::count() const
{
    if (!m_db.isOpen())
        return 0;
    QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM posts"), m_db);
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

void LibraryDatabase::removePost(const QString &filePath)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM posts WHERE file_path = :file_path"));
    q.bindValue(QStringLiteral(":file_path"), filePath);
    if (!q.exec())
        qWarning() << "LibraryDatabase: delete failed:" << q.lastError().text();
}

void LibraryDatabase::updatePostPath(const QString &oldPath, const QString &newPath)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE posts SET file_path = :new_path WHERE file_path = :old_path"));
    q.bindValue(QStringLiteral(":new_path"), newPath);
    q.bindValue(QStringLiteral(":old_path"), oldPath);
    if (!q.exec())
        qWarning() << "LibraryDatabase: update path failed:" << q.lastError().text();
}

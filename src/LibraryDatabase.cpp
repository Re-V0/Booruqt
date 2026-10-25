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
#include <QStringList>
#include <QUrl>

namespace {

constexpr int kSchemaVersion = 2;

QString escapeLike(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return value;
}

// Search terms, each once: post_tags matches without case, so "Solo solo"
// would otherwise count twice and never be satisfied.
QStringList distinctTerms(const QString &tagQuery)
{
    QStringList terms;
    QStringList seen;
    for (const QString &term : tagQuery.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QString lower = term.toLower();
        if (seen.contains(lower))
            continue;
        seen.append(lower);
        terms.append(term);
    }
    return terms;
}

Post postFromQuery(const QSqlQuery &q)
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

} // namespace

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

    q.exec(QStringLiteral("PRAGMA user_version"));
    const int version = q.next() ? q.value(0).toInt() : 0;
    // A statement still open keeps tables locked, and the migration below
    // drops one.
    q.finish();
    if (version < kSchemaVersion) {
        if (!migrateToTagTable())
            qWarning() << "LibraryDatabase: migration to schema" << kSchemaVersion << "failed";
    }
}

bool LibraryDatabase::migrateToTagTable()
{
    if (!m_db.transaction())
        return false;

    QSqlQuery q(m_db);
    // NOCASE: "Solo" and "solo" are one tag, as they are for the blacklist.
    // The primary key also serves "posts having tag X" lookups.
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_tags ("
                       "  post_id INTEGER NOT NULL,"
                       "  tag TEXT NOT NULL COLLATE NOCASE,"
                       "  PRIMARY KEY (tag, post_id)"
                       ") WITHOUT ROWID"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS post_tags_by_post ON post_tags(post_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS posts_by_source_post ON posts(source, post_id)"),
        QStringLiteral("DROP TRIGGER IF EXISTS posts_ai"),
        QStringLiteral("DROP TRIGGER IF EXISTS posts_ad"),
        QStringLiteral("DROP TRIGGER IF EXISTS posts_au"),
        QStringLiteral("DROP TABLE IF EXISTS posts_fts"),
        QStringLiteral("DELETE FROM post_tags"),
    };
    for (const QString &statement : statements) {
        if (!q.exec(statement)) {
            qWarning() << "LibraryDatabase: migration step failed:" << statement << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }

    // Fill post_tags from the tags every existing post already carries.
    QSqlQuery rows(m_db);
    if (!rows.exec(QStringLiteral("SELECT id, tags FROM posts"))) {
        m_db.rollback();
        return false;
    }
    while (rows.next()) {
        if (!replaceTags(rows.value(0).toLongLong(), rows.value(1).toString())) {
            m_db.rollback();
            return false;
        }
    }

    if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion))) {
        m_db.rollback();
        return false;
    }
    return m_db.commit();
}

bool LibraryDatabase::replaceTags(qint64 rowId, const QString &tags)
{
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM post_tags WHERE post_id = :post_id"));
    remove.bindValue(QStringLiteral(":post_id"), rowId);
    if (!remove.exec()) {
        qWarning() << "LibraryDatabase: clearing tags failed:" << remove.lastError().text();
        return false;
    }

    QSqlQuery insert(m_db);
    // OR IGNORE: the same tag twice (or in two cases) is one row.
    insert.prepare(QStringLiteral("INSERT OR IGNORE INTO post_tags (post_id, tag) VALUES (:post_id, :tag)"));
    for (const QString &tag : tags.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        insert.bindValue(QStringLiteral(":post_id"), rowId);
        insert.bindValue(QStringLiteral(":tag"), tag);
        if (!insert.exec()) {
            qWarning() << "LibraryDatabase: inserting tag failed:" << insert.lastError().text();
            return false;
        }
    }
    return true;
}

void LibraryDatabase::addOrUpdatePost(const Post &post, const QString &filePath)
{
    if (!m_db.isOpen())
        return;

    const QString tags = post.tags.join(QLatin1Char(' '));

    if (!m_db.transaction()) {
        qWarning() << "LibraryDatabase: could not start a transaction:" << m_db.lastError().text();
        return;
    }

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
    q.bindValue(QStringLiteral(":tags"), tags);
    q.bindValue(QStringLiteral(":downloaded_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    if (!q.exec()) {
        qWarning() << "LibraryDatabase: insert failed:" << q.lastError().text();
        m_db.rollback();
        return;
    }

    // An upsert that updated an existing row reports no new rowid, so look
    // the row up by the key it was matched on.
    QSqlQuery idQuery(m_db);
    idQuery.prepare(QStringLiteral("SELECT id FROM posts WHERE file_path = :file_path"));
    idQuery.bindValue(QStringLiteral(":file_path"), filePath);
    if (!idQuery.exec() || !idQuery.next()) {
        qWarning() << "LibraryDatabase: inserted row not found:" << idQuery.lastError().text();
        m_db.rollback();
        return;
    }

    if (!replaceTags(idQuery.value(0).toLongLong(), tags)) {
        m_db.rollback();
        return;
    }
    m_db.commit();
}

QVector<Post> LibraryDatabase::search(const QString &tagQuery, int limit, qint64 beforeId,
                                      const QString &dirFilter, bool includeSubdirs) const
{
    QVector<Post> results;
    if (!m_db.isOpen())
        return results;

    const QStringList terms = distinctTerms(tagQuery.trimmed());

    // The folder as a prefix ending in "/", so "/lib" does not match "/library".
    QString dirPrefix;
    if (!dirFilter.isEmpty()) {
        dirPrefix = QDir::cleanPath(dirFilter);
        if (!dirPrefix.endsWith(QLatin1Char('/')))
            dirPrefix += QLatin1Char('/');
    }

    QStringList where;
    if (beforeId > 0)
        where << QStringLiteral("posts.id < :before_id");
    if (!terms.isEmpty()) {
        // A post qualifies when it has every term: one post_tags row matched
        // per term, so the count of matches equals the number of terms.
        QStringList placeholders;
        for (int i = 0; i < terms.size(); ++i)
            placeholders << QStringLiteral(":tag%1").arg(i);
        where << QStringLiteral("posts.id IN (SELECT post_id FROM post_tags WHERE tag IN (%1) "
                                "GROUP BY post_id HAVING COUNT(*) = :tag_count)")
                     .arg(placeholders.join(QStringLiteral(", ")));
    }
    if (!dirPrefix.isEmpty()) {
        // substr() instead of LIKE: LIKE ignores ASCII case, and on Linux
        // "Art/" and "art/" are different folders.
        where << QStringLiteral("substr(posts.file_path, 1, length(:dir_a)) = :dir_b");
        if (!includeSubdirs)
            where << QStringLiteral("instr(substr(posts.file_path, length(:dir_c) + 1), '/') = 0");
    }

    QString sql = QStringLiteral("SELECT posts.* FROM posts");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY posts.id DESC LIMIT :limit");

    QSqlQuery q(m_db);
    q.prepare(sql);
    if (beforeId > 0)
        q.bindValue(QStringLiteral(":before_id"), beforeId);
    for (int i = 0; i < terms.size(); ++i)
        q.bindValue(QStringLiteral(":tag%1").arg(i), terms.at(i));
    if (!terms.isEmpty())
        q.bindValue(QStringLiteral(":tag_count"), int(terms.size()));
    if (!dirPrefix.isEmpty()) {
        q.bindValue(QStringLiteral(":dir_a"), dirPrefix);
        q.bindValue(QStringLiteral(":dir_b"), dirPrefix);
        if (!includeSubdirs)
            q.bindValue(QStringLiteral(":dir_c"), dirPrefix);
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

    // LIKE on a NOCASE column can use the primary key's index.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT tag, COUNT(*) AS uses FROM post_tags "
        "WHERE tag LIKE :pattern ESCAPE '\\' "
        "GROUP BY tag ORDER BY uses DESC, tag LIMIT :limit"));
    q.bindValue(QStringLiteral(":pattern"), escapeLike(prefix) + QLatin1Char('%'));
    q.bindValue(QStringLiteral(":limit"), limit);
    if (!q.exec()) {
        qWarning() << "LibraryDatabase: tag suggestions failed:" << q.lastError().text();
        return {};
    }

    QStringList tags;
    while (q.next())
        tags.append(q.value(0).toString());
    return tags;
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
    if (!m_db.transaction())
        return;

    QSqlQuery tags(m_db);
    tags.prepare(QStringLiteral(
        "DELETE FROM post_tags WHERE post_id IN (SELECT id FROM posts WHERE file_path = :file_path)"));
    tags.bindValue(QStringLiteral(":file_path"), filePath);

    QSqlQuery post(m_db);
    post.prepare(QStringLiteral("DELETE FROM posts WHERE file_path = :file_path"));
    post.bindValue(QStringLiteral(":file_path"), filePath);

    if (!tags.exec() || !post.exec()) {
        qWarning() << "LibraryDatabase: delete failed:" << tags.lastError().text() << post.lastError().text();
        m_db.rollback();
        return;
    }
    m_db.commit();
}

void LibraryDatabase::updatePostPath(const QString &oldPath, const QString &newPath)
{
    if (!m_db.isOpen())
        return;
    // post_tags is keyed by row id, which a path change keeps.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE posts SET file_path = :new_path WHERE file_path = :old_path"));
    q.bindValue(QStringLiteral(":new_path"), newPath);
    q.bindValue(QStringLiteral(":old_path"), oldPath);
    if (!q.exec())
        qWarning() << "LibraryDatabase: update path failed:" << q.lastError().text();
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Pure-logic regression tests: no network, no real user data. Everything
// that touches settings or the database runs under QStandardPaths test mode,
// so running this never disturbs an installed copy.
#include "Blacklist.h"
#include "HtmlEntities.h"
#include "Paging.h"
#include "TagLineEdit.h"
#include "LibraryDatabase.h"
#include "sources/GelbooruSource.h"
#include "sources/DanbooruSource.h"
#include "sources/MoebooruSource.h"
#include "sources/E621Source.h"
#include "sources/PhilomenaSource.h"
#include "sources/SzurubooruSource.h"
#include "sources/PythonPluginSource.h"
#include "NetworkLog.h"
#include "ThumbnailLoader.h"
#include "ThemeManager.h"
#include "DownloadManager.h"
#include "SourceManager.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QStandardPaths>
#include <QDir>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <iostream>

namespace {

int failures = 0;
QString section;

void begin(const char *name)
{
    section = QString::fromLatin1(name);
    std::cout << "\n== " << name << "\n";
}

void check(const char *name, bool ok, const QString &detail = {})
{
    if (!ok)
        ++failures;
    std::cout << (ok ? "  pass  " : "  FAIL  ") << name;
    if (!detail.isEmpty())
        std::cout << "  [" << qPrintable(detail) << "]";
    std::cout << "\n";
}

void equal(const char *name, const QString &got, const QString &want)
{
    check(name, got == want, got == want ? QString() : QStringLiteral("got \"%1\", want \"%2\"").arg(got, want));
}

Post postWith(const QStringList &tags, const QString &source = QStringLiteral("Src"))
{
    Post p;
    p.tags = tags;
    p.sourceName = source;
    return p;
}

void testHtmlEntities()
{
    begin("HTML entities (Gelbooru sends escaped tags)");
    equal("numeric entity", decodeHtmlEntities(QStringLiteral("baldur&#039;s_gate_3")),
          QStringLiteral("baldur's_gate_3"));
    equal("named entity", decodeHtmlEntities(QStringLiteral("dungeons_&amp;_dragons")),
          QStringLiteral("dungeons_&_dragons"));
    equal("hex entity", decodeHtmlEntities(QStringLiteral("&#x27;q&#x27;")), QStringLiteral("'q'"));
    equal("plain text untouched", decodeHtmlEntities(QStringLiteral("no_entities")),
          QStringLiteral("no_entities"));
    equal("malformed left alone", decodeHtmlEntities(QStringLiteral("bad&entity; &#;")),
          QStringLiteral("bad&entity; &#;"));
    equal("out-of-range rejected", decodeHtmlEntities(QStringLiteral("&#999999999999;")),
          QStringLiteral("&#999999999999;"));

    // Above U+FFFF a code point is two UTF-16 units; QChar(code) used to
    // truncate it (or assert in a debug build).
    const char32_t grinning = 0x1F600;
    equal("emoji above the BMP", decodeHtmlEntities(QStringLiteral("smile_&#128512;")),
          QStringLiteral("smile_") + QString::fromUcs4(&grinning, 1));
    equal("hex emoji above the BMP", decodeHtmlEntities(QStringLiteral("&#x1F600;")),
          QString::fromUcs4(&grinning, 1));
    equal("lone surrogate rejected", decodeHtmlEntities(QStringLiteral("&#55357;")),
          QStringLiteral("&#55357;"));
}

void testNetworkLogRedaction()
{
    begin("Network log hides credentials");
    const QUrl url(QStringLiteral(
        "https://gelbooru.com/index.php?page=dapi&tags=1girl&api_key=SECRET123&user_id=4242"));
    const QString shown = NetworkLog::redactedUrl(url);
    check("api_key value hidden", !shown.contains(QStringLiteral("SECRET123")), shown);
    check("user_id value hidden", !shown.contains(QStringLiteral("4242")), shown);
    check("other parameters kept", shown.contains(QStringLiteral("tags=1girl")), shown);

    const QUrl plain(QStringLiteral("https://danbooru.donmai.us/posts.json?tags=1girl&page=2"));
    equal("URL without credentials unchanged", NetworkLog::redactedUrl(plain), plain.toString());
}

void testBlacklist()
{
    begin("Blacklist");
    Blacklist::setGlobalTags({QStringLiteral("Spoiler"), QStringLiteral("  flash "), QStringLiteral("spoiler")});
    equal("normalized and deduplicated", Blacklist::globalTags().join(QLatin1Char(',')),
          QStringLiteral("spoiler,flash"));

    Blacklist::setTagsForSource(QStringLiteral("Danbooru"), {QStringLiteral("wip")});
    equal("per-source merges with global",
          Blacklist::effectiveTags(QStringLiteral("Danbooru")).join(QLatin1Char(',')),
          QStringLiteral("spoiler,flash,wip"));
    equal("other source unaffected",
          Blacklist::effectiveTags(QStringLiteral("Safebooru")).join(QLatin1Char(',')),
          QStringLiteral("spoiler,flash"));

    // A name with a slash must not become a QSettings subgroup.
    Blacklist::setTagsForSource(QStringLiteral("we/ird"), {QStringLiteral("x")});
    equal("slash in source name survives",
          Blacklist::effectiveTags(QStringLiteral("we/ird")).join(QLatin1Char(',')),
          QStringLiteral("spoiler,flash,x"));

    const QStringList rules = {QStringLiteral("solo")};
    check("no substring false positive",
          !Blacklist::blocks(postWith({QStringLiteral("solo_focus"), QStringLiteral("1girl")}), rules));
    check("exact tag blocks", Blacklist::blocks(postWith({QStringLiteral("solo")}), rules));
    check("case-insensitive", Blacklist::blocks(postWith({QStringLiteral("Solo")}), rules));
    check("empty rules block nothing", !Blacklist::blocks(postWith({QStringLiteral("solo")}), {}));

    Blacklist::setTagsForSource(QStringLiteral("Danbooru"), {});
    equal("clearing per-source keeps global",
          Blacklist::effectiveTags(QStringLiteral("Danbooru")).join(QLatin1Char(',')),
          QStringLiteral("spoiler,flash"));

    check("library flag defaults off", !Blacklist::appliesToLibrary());
    Blacklist::setAppliesToLibrary(true);
    check("library flag persists", Blacklist::appliesToLibrary());
    Blacklist::setAppliesToLibrary(false);
}

void testTagInput()
{
    begin("Tag input (token, completion, inline suggestion)");
    equal("token at end", TagLineEdit::tokenAt(QStringLiteral("1girl bald"), 10), QStringLiteral("bald"));
    equal("token under caret, not last word",
          TagLineEdit::tokenAt(QStringLiteral("1girl bald solo"), 10), QStringLiteral("bald"));
    equal("caret at start", TagLineEdit::tokenAt(QStringLiteral("bald"), 0), QString());
    equal("caret after separator", TagLineEdit::tokenAt(QStringLiteral("1girl "), 6), QString());

    {
        TagLineEdit edit;
        edit.show();
        edit.setText(QStringLiteral("1girl bald solo"));
        edit.setCursorPosition(10);
        edit.acceptCompletion(QStringLiteral("baldurs_gate"));
        equal("mid-query completion keeps the rest", edit.text(), QStringLiteral("1girl baldurs_gate solo"));
        check("no doubled separator", !edit.text().contains(QStringLiteral("  ")));
    }
    {
        TagLineEdit edit;
        edit.show();
        edit.setText(QStringLiteral("1girl bald"));
        edit.setCursorPosition(10);
        edit.acceptCompletion(QStringLiteral("baldurs_gate"));
        equal("completion at end appends a separator", edit.text(), QStringLiteral("1girl baldurs_gate "));
    }
    {
        // Both ways of accepting a tag must look like an edit, or the next
        // round of suggestions is never requested after using the popup.
        TagLineEdit edit;
        edit.show();
        int edits = 0;
        QObject::connect(&edit, &QLineEdit::textEdited, [&edits]() { ++edits; });

        edit.setText(QStringLiteral("bal"));
        edit.setCursorPosition(3);
        edit.acceptCompletion(QStringLiteral("baldurs_gate"));
        check("popup completion emits textEdited", edits == 1, QStringLiteral("%1 signals").arg(edits));

        edit.setSuggestion(QStringLiteral("baldurs_gate_3"));
        edit.setText(QStringLiteral("baldurs_gate_"));
        edit.setCursorPosition(13);
        edit.setSuggestion(QStringLiteral("baldurs_gate_3"));
        edits = 0;
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&edit, &right);
        check("inline completion emits textEdited too", edits == 1, QStringLiteral("%1 signals").arg(edits));
    }
    {
        // The completer popup takes focus; the pending suggestion must survive.
        TagLineEdit edit;
        edit.show();
        edit.setFocus();
        edit.setText(QStringLiteral("bald"));
        edit.setCursorPosition(4);
        edit.setSuggestion(QStringLiteral("baldurs_gate"));
        QFocusEvent focusOut(QEvent::FocusOut, Qt::PopupFocusReason);
        QApplication::sendEvent(&edit, &focusOut);
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&edit, &right);
        equal("ghost survives popup focus and Right accepts it", edit.text(), QStringLiteral("baldurs_gate"));
    }
    {
        TagLineEdit edit;
        edit.show();
        edit.setFocus();
        edit.setText(QStringLiteral("bald"));
        edit.setCursorPosition(4);
        edit.setSuggestion(QStringLiteral("baldurs_gate"));
        QFocusEvent focusOut(QEvent::FocusOut, Qt::MouseFocusReason);
        QApplication::sendEvent(&edit, &focusOut);
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&edit, &right);
        equal("leaving the field drops the ghost", edit.text(), QStringLiteral("bald"));
    }
}

void testSources()
{
    begin("Source flavors");
    GelbooruSource gelbooru(QStringLiteral("Safebooru"), QStringLiteral("https://safebooru.org"));
    DanbooruSource danbooru(QStringLiteral("Danbooru"), QStringLiteral("https://danbooru.donmai.us"));
    MoebooruSource moebooru(QStringLiteral("Yande.re"), QStringLiteral("https://yande.re"));
    E621Source e621(QStringLiteral("e621"), QStringLiteral("https://e621.net"));
    PhilomenaSource philomena(QStringLiteral("Derpibooru"), QStringLiteral("https://derpibooru.org"));

    // Verified against the live APIs; Danbooru rejects >2 tags for anonymous
    // users, so its blacklist has to stay client-side.
    check("gelbooru excludes server-side", gelbooru.supportsTagExclusion());
    check("moebooru excludes server-side", moebooru.supportsTagExclusion());
    check("e621 excludes server-side", e621.supportsTagExclusion());
    check("philomena excludes server-side", philomena.supportsTagExclusion());
    check("danbooru stays client-side", !danbooru.supportsTagExclusion());

    GelbooruSource bridged(QStringLiteral("scraped"), QStringLiteral("https://example.org"), {}, {}, {}, {},
                           QStringLiteral("http://127.0.0.1:8765"));
    check("scrape bridge stays client-side", !bridged.supportsTagExclusion());

    // Checked on the encoded form: that is what actually goes over the wire.
    const QString url = QString::fromLatin1(
        philomena.buildSearchRequest(QStringLiteral("1girl -spoiler"), 1, 5).url().toEncoded());
    check("philomena joins tags with commas", url.contains(QStringLiteral("q=1girl,%20-spoiler")), url);

    const QByteArray gelbooruSuggestions =
        R"JSON([{"label":"baldur&#039;s_gate (1)","value":"baldur&#039;s_gate"}])JSON";
    equal("gelbooru suggestions are unescaped",
          gelbooru.parseTagSuggestions(gelbooruSuggestions).join(QLatin1Char(',')),
          QStringLiteral("baldur's_gate"));

    const QByteArray danbooruSuggestions =
        R"JSON([{"value":"baldurs_gate","label":"baldurs gate"},{"value":"baldurs_gate_3"}])JSON";
    equal("danbooru suggestions parsed",
          danbooru.parseTagSuggestions(danbooruSuggestions).join(QLatin1Char(',')),
          QStringLiteral("baldurs_gate,baldurs_gate_3"));

    const QByteArray philomenaSuggestions = R"JSON({"tags":[{"name":"applejack"},{"name":"apple"}]})JSON";
    equal("philomena suggestions parsed",
          philomena.parseTagSuggestions(philomenaSuggestions).join(QLatin1Char(',')),
          QStringLiteral("applejack,apple"));

    // Philomena tags have spaces; the app separates tags with spaces. "_"
    // stands for the space on our side, in both directions.
    const QString multiWord = QString::fromLatin1(
        philomena.buildSearchRequest(QStringLiteral("twilight_sparkle -princess_celestia"), 1, 5).url().toEncoded());
    check("philomena sends multi-word tags with spaces",
          multiWord.contains(QStringLiteral("q=twilight%20sparkle,%20-princess%20celestia")), multiWord);
    const QByteArray philomenaPosts = R"JSON({"images":[{"id":5,"tags":["twilight sparkle","safe"],
        "view_url":"https://x/f.png","representations":{"thumb":"https://x/t.png"}}]})JSON";
    const QVector<Post> philomenaParsed = philomena.parseSearchResponse(philomenaPosts);
    if (!philomenaParsed.isEmpty()) {
        equal("philomena tags come back as one token each", philomenaParsed.first().tags.join(QLatin1Char(',')),
              QStringLiteral("twilight_sparkle,safe"));
        equal("philomena rating still found", philomenaParsed.first().rating, QStringLiteral("safe"));
    } else {
        check("philomena post parsed", false);
    }
    equal("philomena suggestions use _ for spaces",
          philomena.parseTagSuggestions(R"JSON({"tags":[{"name":"twilight sparkle"}]})JSON").join(QLatin1Char(',')),
          QStringLiteral("twilight_sparkle"));
    const QString perPage = QString::fromLatin1(
        philomena.buildSearchRequest(QStringLiteral("x"), 1, 200).url().toEncoded());
    check("philomena page size capped at the API's 50", perPage.contains(QStringLiteral("per_page=50")), perPage);

    const QByteArray gelbooruPosts = R"JSON([{"id":1,"tags":"baldur&#039;s_gate_3 1girl",
        "file_url":"https://x/a.jpg","preview_url":"https://x/t.jpg"}])JSON";
    const QVector<Post> posts = gelbooru.parseSearchResponse(gelbooruPosts);
    check("gelbooru post parsed", posts.size() == 1, QString::number(posts.size()));
    if (!posts.isEmpty()) {
        equal("post tags are unescaped", posts.first().tags.join(QLatin1Char(',')),
              QStringLiteral("baldur's_gate_3,1girl"));
    }

    // TBIB's dapi sends no URLs at all, and its "image" (sha1) differs from
    // "hash" (md5). Thumbnails and samples are named after the image, as the
    // site's own HTML shows; building them from "hash" gave a 404 for every
    // post. Shape copied from a live response.
    GelbooruSource tbib(QStringLiteral("TBIB"), QStringLiteral("https://tbib.org"));
    const QByteArray tbibPosts = R"JSON([{"directory":8234,"hash":"d6400490154c7a853bcc303f14785d0c",
        "id":28821681,"image":"43e6bfebe7e5431a845014cfd002c49e73285df9.png","sample":true,
        "tags":"1girl solo","rating":"questionable","score":null,"width":2250,"height":3000}])JSON";
    const QVector<Post> tbibParsed = tbib.parseSearchResponse(tbibPosts);
    check("tbib post parsed", tbibParsed.size() == 1, QString::number(tbibParsed.size()));
    if (!tbibParsed.isEmpty()) {
        const Post &p = tbibParsed.first();
        equal("tbib id", p.id, QStringLiteral("28821681"));
        equal("tbib thumbnail named after the image",
              p.previewUrl,
              QStringLiteral("https://tbib.org/thumbnails/8234/thumbnail_43e6bfebe7e5431a845014cfd002c49e73285df9.jpg"));
        equal("tbib sample named after the image",
              p.sampleUrl,
              QStringLiteral("https://tbib.org/samples/8234/sample_43e6bfebe7e5431a845014cfd002c49e73285df9.jpg"));
        equal("tbib original",
              p.fileUrl,
              QStringLiteral("https://tbib.org/images/8234/43e6bfebe7e5431a845014cfd002c49e73285df9.png"));
    }

    // Szurubooru's API docs require "Accept: application/json".
    SzurubooruSource szuru(QStringLiteral("Szuru"), QStringLiteral("https://host.example/booru"));
    const QNetworkRequest szuruRequest = szuru.buildSearchRequest(QStringLiteral("1girl"), 1, 5);
    equal("szurubooru sends Accept", QString::fromLatin1(szuruRequest.rawHeader("Accept")),
          QStringLiteral("application/json"));

    const QByteArray szuruPosts = R"JSON({"results":[{"id":7,"contentUrl":"data/posts/7_a.png",
        "thumbnailUrl":"data/generated-thumbnails/7_a.jpg","tags":[]}]})JSON";
    const QVector<Post> szuruParsed = szuru.parseSearchResponse(szuruPosts);
    check("szurubooru post parsed", szuruParsed.size() == 1, QString::number(szuruParsed.size()));
    if (!szuruParsed.isEmpty()) {
        equal("szurubooru in a subpath keeps the subpath", szuruParsed.first().fileUrl,
              QStringLiteral("https://host.example/booru/data/posts/7_a.png"));
    }
}

void testLibraryDatabase()
{
    begin("Library database");
    LibraryDatabase db;
    check("database opens", db.isOpen());
    if (!db.isOpen())
        return;

    const QString root = QDir::tempPath() + QStringLiteral("/blairqt-test-lib");
    auto add = [&db](const QString &path, const QString &tags, const QString &id) {
        Post p;
        p.id = id;
        p.sourceName = QStringLiteral("Src");
        p.tags = tags.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        db.addOrUpdatePost(p, path);
    };
    add(root + QStringLiteral("/a.jpg"), QStringLiteral("hatsune_miku_(vocaloid) 1girl"), QStringLiteral("1"));
    add(root + QStringLiteral("/sub/b.jpg"), QStringLiteral("hatsune_miku_(vocaloid)"), QStringLiteral("2"));
    add(root + QStringLiteral("/sub/deep/c.jpg"), QStringLiteral("1girl"), QStringLiteral("3"));

    // Parentheses and colons are FTS5 syntax; unquoted they error the query out.
    check("tag with parentheses matches",
          db.search(QStringLiteral("hatsune_miku_(vocaloid)"), -1, 0).size() == 2,
          QString::number(db.search(QStringLiteral("hatsune_miku_(vocaloid)"), -1, 0).size()));
    check("colon tag does not break the query", db.search(QStringLiteral("rating:safe"), -1, 0).isEmpty());

    check("folder filter excludes subfolders", db.search(QString(), -1, 0, root, false).size() == 1,
          QString::number(db.search(QString(), -1, 0, root, false).size()));
    check("folder filter can recurse", db.search(QString(), -1, 0, root, true).size() == 3,
          QString::number(db.search(QString(), -1, 0, root, true).size()));

    equal("known post is found by source and id", db.pathForPost(QStringLiteral("Src"), QStringLiteral("2")),
          root + QStringLiteral("/sub/b.jpg"));
    check("unknown post returns nothing",
          db.pathForPost(QStringLiteral("Src"), QStringLiteral("999")).isEmpty());

    equal("tag suggestions are prefix-matched",
          db.suggestTags(QStringLiteral("hat"), 10).join(QLatin1Char(',')),
          QStringLiteral("hatsune_miku_(vocaloid)"));
    check("suggestions ignore non-prefix matches", db.suggestTags(QStringLiteral("girl"), 10).isEmpty());

    check("rows carry their database id", db.search(QString(), 1, 0).first().libraryId > 0);

    // Keyed paging: page 2 must not re-show or skip rows just because page 1
    // lost an entry to a delete or a move in between.
    {
        const QVector<Post> firstPage = db.search(QString(), 2, 0);
        check("first page is full", firstPage.size() == 2, QString::number(firstPage.size()));
        db.removePost(firstPage.first().localFilePath);

        const QVector<Post> secondPage = db.search(QString(), 2, firstPage.last().libraryId);
        check("page after a deletion returns the remaining row", secondPage.size() == 1,
              QString::number(secondPage.size()));
        QStringList seen;
        for (const Post &p : firstPage + secondPage)
            seen << p.id;
        seen.sort();
        equal("every row appears exactly once across pages", seen.join(QLatin1Char(',')),
              QStringLiteral("1,2,3"));

        check("past the last row there is nothing left",
              db.search(QString(), 2, secondPage.last().libraryId).isEmpty());

        // Restore the row the deletion test removed.
        add(firstPage.first().localFilePath, firstPage.first().tags.join(QLatin1Char(' ')),
            firstPage.first().id);
    }

    db.removePost(root + QStringLiteral("/a.jpg"));
    check("removed post is gone", db.count() == 2, QString::number(db.count()));
}

void testLibraryExactTags()
{
    begin("Library search matches whole tags");
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    LibraryDatabase db;
    if (!db.isOpen()) {
        check("database opens", false);
        return;
    }

    const QString root = QDir::tempPath() + QStringLiteral("/blairqt-test-exact");
    auto add = [&db](const QString &path, const QString &tags, const QString &id) {
        Post p;
        p.id = id;
        p.sourceName = QStringLiteral("Exact");
        p.tags = tags.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        db.addOrUpdatePost(p, path);
    };
    add(root + QStringLiteral("/art/1.jpg"), QStringLiteral("solo_focus 1girl"), QStringLiteral("e1"));
    add(root + QStringLiteral("/Art/2.jpg"), QStringLiteral("hat red dress"), QStringLiteral("e2"));
    add(root + QStringLiteral("/art/3.jpg"), QStringLiteral("Solo red_dress"), QStringLiteral("e3"));

    auto ids = [](const QVector<Post> &posts) {
        QStringList out;
        for (const Post &p : posts)
            out << p.id;
        out.sort();
        return out.join(QLatin1Char(','));
    };

    // FTS5 split tags at "_", so these used to match the wrong posts.
    equal("solo does not find solo_focus", ids(db.search(QStringLiteral("solo"), -1)), QStringLiteral("e3"));
    equal("red_dress does not find separate red and dress", ids(db.search(QStringLiteral("red_dress"), -1)),
          QStringLiteral("e3"));
    equal("all terms are required", ids(db.search(QStringLiteral("red dress"), -1)), QStringLiteral("e2"));
    equal("case does not matter", ids(db.search(QStringLiteral("SOLO_FOCUS"), -1)), QStringLiteral("e1"));
    equal("a repeated term counts once", ids(db.search(QStringLiteral("solo Solo"), -1)), QStringLiteral("e3"));

    // LIKE ignored ASCII case: "art" and "Art" are different folders on Linux.
    equal("folder filter respects case", ids(db.search(QString(), -1, 0, root + QStringLiteral("/art"))),
          QStringLiteral("e1,e3"));
    equal("folder name is not a prefix match",
          ids(db.search(QString(), -1, 0, root + QStringLiteral("/ar"), true)), QString());

    // Re-saving a post replaces its tags rather than adding to them.
    add(root + QStringLiteral("/art/1.jpg"), QStringLiteral("landscape"), QStringLiteral("e1"));
    check("old tags gone after an update", db.search(QStringLiteral("solo_focus"), -1).isEmpty());
    equal("new tags searchable after an update", ids(db.search(QStringLiteral("landscape"), -1)),
          QStringLiteral("e1"));

    db.removePost(root + QStringLiteral("/art/3.jpg"));
    check("removed post leaves no tags behind", db.suggestTags(QStringLiteral("red_d"), 10).isEmpty());
}

void testSourceManager()
{
    begin("Source list");
    SourceManager manager;
    QSharedPointer<ISource> builtin = manager.byName(QStringLiteral("Danbooru"));
    check("built-in Danbooru present", builtin && builtin->flavor() == QStringLiteral("danbooru"));

    // A custom source with a built-in's name replaces it...
    manager.addCustomSource(QStringLiteral("Danbooru"), QStringLiteral("moebooru"),
                            QStringLiteral("https://mirror.example"), QStringLiteral("secret-key"));
    QSharedPointer<ISource> custom = manager.byName(QStringLiteral("Danbooru"));
    check("custom source shadows the built-in", custom && custom->flavor() == QStringLiteral("moebooru"));

#ifdef Q_OS_UNIX
    const QString file = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/custom_sources.json");
    const QFileDevice::Permissions permissions = QFile::permissions(file);
    check("file with API keys is owner-only",
          !(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther)),
          QString::number(int(permissions), 16));
#endif

    // ...and removing it brings the built-in back at once, not after a restart.
    manager.removeSource(QStringLiteral("Danbooru"));
    QSharedPointer<ISource> restored = manager.byName(QStringLiteral("Danbooru"));
    check("built-in back after removing the custom one",
          restored && restored->flavor() == QStringLiteral("danbooru"));

    SourceManager reloaded;
    QSharedPointer<ISource> afterRestart = reloaded.byName(QStringLiteral("Danbooru"));
    check("and still there after a restart", afterRestart && afterRestart->flavor() == QStringLiteral("danbooru"));
}

void testLibraryPathBatch()
{
    begin("Library path updates in one transaction");
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    LibraryDatabase db;
    Post p;
    p.sourceName = QStringLiteral("Batch");
    p.tags = {QStringLiteral("batch_tag")};
    QVector<QPair<QString, QString>> moves;
    for (int i = 0; i < 5; ++i) {
        p.id = QString::number(i);
        const QString oldPath = QStringLiteral("/batch/old/%1.jpg").arg(i);
        db.addOrUpdatePost(p, oldPath);
        moves.append({oldPath, QStringLiteral("/batch/new/%1.jpg").arg(i)});
    }
    db.updatePostPaths(moves);
    check("all paths moved", db.search(QString(), -1, 0, QStringLiteral("/batch/new")).size() == 5);
    check("nothing left at the old paths", db.search(QString(), -1, 0, QStringLiteral("/batch/old")).isEmpty());
    check("tags survive the move", db.search(QStringLiteral("batch_tag"), -1).size() == 5);
}

void testLibraryMigration()
{
    begin("Library database from 0.3.1 and earlier");
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/library.sqlite3");
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(path);

    // The schema and triggers exactly as 0.3.1 created them.
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy"));
        legacy.setDatabaseName(path);
        check("legacy database opens", legacy.open());
        QSqlQuery q(legacy);
        q.exec(QStringLiteral(
            "CREATE TABLE posts (id INTEGER PRIMARY KEY AUTOINCREMENT, source TEXT NOT NULL,"
            " post_id TEXT NOT NULL, file_path TEXT NOT NULL UNIQUE, file_url TEXT, preview_url TEXT,"
            " rating TEXT, score INTEGER, width INTEGER, height INTEGER, source_url TEXT,"
            " post_page_url TEXT, tags TEXT, downloaded_at TEXT NOT NULL)"));
        q.exec(QStringLiteral("CREATE VIRTUAL TABLE posts_fts USING fts5(tags, content='posts', content_rowid='id')"));
        q.exec(QStringLiteral("CREATE TRIGGER posts_ai AFTER INSERT ON posts BEGIN"
                              " INSERT INTO posts_fts(rowid, tags) VALUES (new.id, new.tags); END"));
        q.exec(QStringLiteral("CREATE TRIGGER posts_ad AFTER DELETE ON posts BEGIN"
                              " INSERT INTO posts_fts(posts_fts, rowid, tags) VALUES('delete', old.id, old.tags); END"));
        q.exec(QStringLiteral("CREATE TRIGGER posts_au AFTER UPDATE ON posts BEGIN"
                              " INSERT INTO posts_fts(posts_fts, rowid, tags) VALUES('delete', old.id, old.tags);"
                              " INSERT INTO posts_fts(rowid, tags) VALUES (new.id, new.tags); END"));
        q.exec(QStringLiteral("INSERT INTO posts (source, post_id, file_path, tags, downloaded_at)"
                              " VALUES ('Old', '1', '/old/a.jpg', 'solo_focus 1girl', 'x'),"
                              "        ('Old', '2', '/old/b.jpg', 'solo', 'x')"));
        legacy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy"));

    {
        LibraryDatabase db;
        check("migrated database opens", db.isOpen());
        check("existing posts kept", db.count() == 2, QString::number(db.count()));
        const QVector<Post> solo = db.search(QStringLiteral("solo"), -1);
        check("existing tags searchable, exactly", solo.size() == 1 && solo.first().id == QStringLiteral("2"),
              QString::number(solo.size()));
        equal("existing tags suggested", db.suggestTags(QStringLiteral("solo_"), 10).join(QLatin1Char(',')),
              QStringLiteral("solo_focus"));

        // The dropped FTS triggers must not fire any more on writes.
        Post p;
        p.id = QStringLiteral("3");
        p.sourceName = QStringLiteral("Old");
        p.tags = {QStringLiteral("new_tag")};
        db.addOrUpdatePost(p, QStringLiteral("/old/c.jpg"));
        check("writes work after migration", db.search(QStringLiteral("new_tag"), -1).size() == 1);
    }

    // Opening again must not migrate twice and lose or duplicate anything.
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    {
        LibraryDatabase db;
        check("second open keeps all posts", db.count() == 3, QString::number(db.count()));
        check("second open keeps tags", db.search(QStringLiteral("1girl"), -1).size() == 1);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(path);
}

void testViewerUrls()
{
    begin("Viewer URLs (video vs image)");
    check("mp4 recognised", isVideoUrl(QStringLiteral("https://x.org/a.mp4")));
    check("webm recognised", isVideoUrl(QStringLiteral("https://x.org/a.webm?token=1")));
    check("local video recognised", isVideoUrl(QStringLiteral("file:///home/u/clip.MKV")));
    check("jpg is not video", !isVideoUrl(QStringLiteral("https://x.org/a.jpg")));
    check("query string alone does not fool it",
          !isVideoUrl(QStringLiteral("https://x.org/a.jpg?src=movie.mp4")));

    Post video;
    video.sampleUrl = QStringLiteral("https://x.org/sample.mp4");
    video.fileUrl = QStringLiteral("https://x.org/full.webm");
    video.previewUrl = QStringLiteral("https://x.org/thumb.jpg");
    equal("video post falls back to the still thumbnail", viewerImageUrl(video),
          QStringLiteral("https://x.org/thumb.jpg"));

    Post localVideo;
    localVideo.localFilePath = QStringLiteral("/home/u/clip.webm");
    localVideo.previewUrl = QUrl::fromLocalFile(localVideo.localFilePath).toString();
    check("all-video post offers nothing to zoom into", viewerImageUrl(localVideo).isEmpty());

    Post image;
    image.localFilePath = QStringLiteral("/home/u/pic.png");
    image.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    equal("local image still wins", viewerImageUrl(image),
          QUrl::fromLocalFile(QStringLiteral("/home/u/pic.png")).toString());

    Post remote;
    remote.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    remote.fileUrl = QStringLiteral("https://x.org/full.png");
    equal("remote viewer prefers the sample", viewerImageUrl(remote), remote.sampleUrl);
    equal("downloads still take the original", fullImageUrl(remote), remote.fileUrl);
}

void testAutoLoadChain()
{
    begin("Infinite scroll self-loading");
    constexpr int limit = 5;
    check("off when infinite scroll is off", !shouldChainAutoLoad(false, 20, 0, 0, 0, limit));
    check("a fully blacklisted page pulls the next one",
          shouldChainAutoLoad(true, 20, 0, 0, 0, limit));
    check("a page too short to scroll pulls the next one",
          shouldChainAutoLoad(true, 20, 20, 0, 0, limit));
    check("a scrollable page waits for the user",
          !shouldChainAutoLoad(true, 20, 20, 900, 0, limit));
    check("end of results stops it", !shouldChainAutoLoad(true, 0, 0, 0, 0, limit));
    check("the limit stops a runaway", !shouldChainAutoLoad(true, 20, 0, 0, limit, limit));
    check("one below the limit still runs", shouldChainAutoLoad(true, 20, 0, 0, limit - 1, limit));
}

void testTagExclusionComposition()
{
    begin("Blacklist as search terms");
    const QStringList blocked = {QStringLiteral("wip"), QStringLiteral("spoiler")};

    MoebooruSource honours(QStringLiteral("yande.re"), QStringLiteral("https://yande.re"));
    equal("excluded tags are appended for sites that honour them",
          searchTagsWithExclusions(honours, QStringLiteral("1girl"), blocked),
          QStringLiteral("1girl -wip -spoiler"));

    SzurubooruSource ignores(QStringLiteral("Szuru"), QStringLiteral("https://szuru.example"));
    equal("other sites keep the query untouched",
          searchTagsWithExclusions(ignores, QStringLiteral("1girl"), blocked),
          QStringLiteral("1girl"));

    equal("an empty blacklist changes nothing",
          searchTagsWithExclusions(honours, QStringLiteral("1girl"), {}),
          QStringLiteral("1girl"));
}

void testPluginSource()
{
    begin("Python plugin source (must not block the GUI thread)");

    QTemporaryDir dir;
    if (!dir.isValid()) {
        check("temporary directory", false);
        return;
    }
    const QString scriptPath = dir.filePath(QStringLiteral("slow_plugin.py"));
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly)) {
        check("plugin script written", false);
        return;
    }
    script.write(
        "import time\n"
        "def build_search_request(base_url, tags, page, per_page):\n"
        "    time.sleep(0.6)\n"
        "    return {'url': f'{base_url}/x?q={tags}'}\n"
        "def parse_response(body, base_url):\n"
        "    return []\n"
        "def supports_tag_exclusion():\n"
        "    return True\n");
    script.close();

    PythonPluginSource source(QStringLiteral("Test"), QStringLiteral("https://example.org"), scriptPath);
    check("declares itself blocking", source.mayBlock());
    check("capability read from the script", source.supportsTagExclusion());

    // The app runs blocking sources through QtConcurrent; prove that pattern
    // leaves the event loop free by counting timer ticks during the call.
    int ticks = 0;
    QTimer ticker;
    ticker.setInterval(50);
    QObject::connect(&ticker, &QTimer::timeout, [&ticks]() { ++ticks; });
    ticker.start();

    QFuture<QNetworkRequest> future = QtConcurrent::run([&source]() {
        return source.buildSearchRequest(QStringLiteral("1girl"), 1, 5);
    });
    while (!future.isFinished())
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    ticker.stop();

    check("event loop kept running while the plugin worked", ticks >= 3, QStringLiteral("%1 ticks").arg(ticks));
    equal("plugin built the request", future.result().url().toString(),
          QStringLiteral("https://example.org/x?q=1girl"));

    // The capability answer comes from a spawned process, so it is cached;
    // otherwise every search would pay for it again.
    QElapsedTimer cached;
    cached.start();
    source.supportsTagExclusion();
    check("capability is answered from cache after the first ask", cached.elapsed() < 50,
          QStringLiteral("%1 ms").arg(cached.elapsed()));

    // The whole composition, capability query included, has to happen on the
    // worker - that is exactly what MainWindow hands to QtConcurrent.
    const QString slowScriptPath = dir.filePath(QStringLiteral("slow_caps.py"));
    QFile slow(slowScriptPath);
    if (!slow.open(QIODevice::WriteOnly)) {
        check("slow plugin script written", false);
        return;
    }
    slow.write(
        "import time\n"
        "def build_search_request(base_url, tags, page, per_page):\n"
        "    return {'url': f'{base_url}/x?q={tags}'}\n"
        "def parse_response(body, base_url):\n"
        "    return []\n"
        "def supports_tag_exclusion():\n"
        "    time.sleep(0.6)\n"
        "    return True\n");
    slow.close();

    PythonPluginSource slowSource(QStringLiteral("Slow"), QStringLiteral("https://example.org"), slowScriptPath);
    ticks = 0;
    ticker.start();
    QFuture<QNetworkRequest> composed = QtConcurrent::run([&slowSource]() {
        const QString tags = searchTagsWithExclusions(slowSource, QStringLiteral("1girl"),
                                                      {QStringLiteral("spoiler")});
        return slowSource.buildSearchRequest(tags, 1, 5);
    });
    while (!composed.isFinished())
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    ticker.stop();

    check("event loop survived the capability query too", ticks >= 3, QStringLiteral("%1 ticks").arg(ticks));
    equal("blacklist reached the plugin request", composed.result().url().toString(),
          QStringLiteral("https://example.org/x?q=1girl -spoiler"));

    // "id": 42 is what a Python plugin naturally returns; it used to be
    // turned into "" and the post silently dropped.
    const QString intIdPath = dir.filePath(QStringLiteral("int_id.py"));
    QFile intId(intIdPath);
    if (!intId.open(QIODevice::WriteOnly)) {
        check("int-id plugin script written", false);
        return;
    }
    intId.write(
        "def build_search_request(base_url, tags, page, per_page):\n"
        "    return {'url': base_url}\n"
        "def parse_response(body, base_url):\n"
        "    return [{'id': 42, 'file_url': 'https://example.org/a.png'},\n"
        "            {'id': 'abc', 'file_url': 'https://example.org/b.png'},\n"
        "            {'file_url': 'https://example.org/no-id.png'}]\n");
    intId.close();

    PythonPluginSource intIdSource(QStringLiteral("IntId"), QStringLiteral("https://example.org"), intIdPath);
    const QVector<Post> intIdPosts = intIdSource.parseSearchResponse("{}");
    check("numeric and string ids both kept, missing id dropped", intIdPosts.size() == 2,
          QString::number(intIdPosts.size()));
    if (intIdPosts.size() == 2) {
        equal("numeric id converted", intIdPosts.at(0).id, QStringLiteral("42"));
        equal("string id unchanged", intIdPosts.at(1).id, QStringLiteral("abc"));
    }
}

void testThumbnailCancel()
{
    begin("Thumbnail loader cancels every request");

    // A server that accepts connections and never answers, so every request
    // stays in flight until cancelAll().
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        check("local test server", false);
        return;
    }
    // Only a connection that carried a request can hold one open; Qt may also
    // keep idle connections around for reuse, which block nothing.
    QList<QTcpSocket *> sockets;
    QSet<QTcpSocket *> carriedRequest;
    QObject::connect(&server, &QTcpServer::newConnection, [&server, &sockets, &carriedRequest]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            // Drain what arrives: a socket with unread data is not polled for
            // EOF, so a client-side close would go unnoticed.
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &carriedRequest]() {
                if (socket->readAll().contains("GET "))
                    carriedRequest.insert(socket);
            });
            sockets.append(socket);
        }
    });

    ThumbnailLoader loader;
    int delivered = 0;
    QObject::connect(&loader, &ThumbnailLoader::thumbnailReady, [&delivered]() { ++delivered; });

    // Distinct hosts get separate connection pools, so all 40 really connect.
    const int total = 40;
    for (int i = 0; i < total; ++i) {
        const QString host = (i % 2 == 0) ? QStringLiteral("127.0.0.1") : QStringLiteral("localhost");
        loader.request(i, QUrl(QStringLiteral("http://%1:%2/t%3.jpg").arg(host).arg(server.serverPort()).arg(i)));
    }

    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 1500)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    const int connected = int(sockets.size());

    loader.cancelAll();

    // An aborted request closes its connection; a skipped one stays open
    // with its request still pending.
    waited.restart();
    while (waited.elapsed() < 1500)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    int stillPending = 0;
    for (QTcpSocket *socket : std::as_const(sockets)) {
        if (socket->state() == QAbstractSocket::ConnectedState && carriedRequest.contains(socket))
            ++stillPending;
    }
    check("requests reached the server", !carriedRequest.isEmpty(),
          QStringLiteral("%1 connections, %2 with a request").arg(connected).arg(carriedRequest.size()));
    check("no request left pending after cancelAll()", stillPending == 0,
          QStringLiteral("%1 of %2 still pending").arg(stillPending).arg(carriedRequest.size()));
    check("nothing delivered after cancelAll()", delivered == 0, QString::number(delivered));
}

// Minimal HTTP server for the download tests. Answers by path:
//   /ok.png    200, image/png, full body
//   /page.png  200, text/html (what a Cloudflare challenge looks like)
//   /cut.png   promises 100000 bytes, sends 10, closes
class TestHttpServer : public QTcpServer {
public:
    TestHttpServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [socket]() { answer(socket); });
            }
        });
        listen(QHostAddress::LocalHost, 0);
    }

    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path);
    }

    static QByteArray okBody() { return QByteArray("\x89PNG\r\n\x1a\nfake image data", 24); }

private:
    static void answer(QTcpSocket *socket)
    {
        const QByteArray request = socket->readAll();
        const QByteArray firstLine = request.left(request.indexOf('\n'));
        QByteArray response;
        if (firstLine.contains(" /ok.png ")) {
            const QByteArray body = okBody();
            response = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else if (firstLine.contains(" /page.png ")) {
            const QByteArray body = "<html><body>Checking your browser...</body></html>";
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else if (firstLine.contains(" /cut.png ")) {
            response = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: 100000\r\n\r\n0123456789";
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }
};

void testDownloads()
{
    begin("Downloads are written whole or not at all");

    QTemporaryDir dir;
    TestHttpServer server;
    if (!dir.isValid() || !server.isListening()) {
        check("temporary directory and test server", false);
        return;
    }

    DownloadManager manager(nullptr);
    manager.setDownloadDir(dir.path());

    // Runs one download and waits for its result.
    struct Result {
        bool finished = false;
        bool ok = false;
        QString path;
        QString error;
    };
    auto run = [&manager](const QString &id, const QString &url) {
        Result result;
        QMetaObject::Connection connection = QObject::connect(&manager, &DownloadManager::downloadFinished,
            [&result](const QString &path, bool ok, bool, const QString &error) {
                result.finished = true;
                result.ok = ok;
                result.path = path;
                result.error = error;
            });
        Post post;
        post.id = id;
        post.sourceName = QStringLiteral("Test");
        post.fileUrl = url;
        manager.download(post);
        QElapsedTimer waited;
        waited.start();
        while (!result.finished && waited.elapsed() < 10000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        QObject::disconnect(connection);
        return result;
    };
    // Everything in the folder, temporary QSaveFile leftovers included.
    auto filesInDir = [&dir]() {
        return QDir(dir.path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    };

    const Result ok = run(QStringLiteral("1"), server.url(QStringLiteral("/ok.png")));
    check("image downloaded", ok.finished && ok.ok, ok.error);
    QFile saved(dir.filePath(QStringLiteral("Test_1.png")));
    check("file has the full body", saved.open(QIODevice::ReadOnly) && saved.readAll() == TestHttpServer::okBody());

    const Result page = run(QStringLiteral("2"), server.url(QStringLiteral("/page.png")));
    check("web page instead of image is refused", page.finished && !page.ok, page.error);
    check("no file left for the refused page", !QFile::exists(dir.filePath(QStringLiteral("Test_2.png"))));

    const Result cut = run(QStringLiteral("3"), server.url(QStringLiteral("/cut.png")));
    check("broken transfer reported", cut.finished && !cut.ok, cut.error);
    check("no half file left behind", !QFile::exists(dir.filePath(QStringLiteral("Test_3.png"))));

    // A "/" in an id must not point the file into another folder.
    const Result slash = run(QStringLiteral("a/b"), server.url(QStringLiteral("/ok.png")));
    check("id with a slash downloads", slash.finished && slash.ok, slash.error);
    check("slash replaced in the file name", QFile::exists(dir.filePath(QStringLiteral("Test_a_b.png"))));

    equal("only the two finished files remain", filesInDir().join(QLatin1Char(',')),
          QStringLiteral("Test_1.png,Test_a_b.png"));
}

// A file the test cannot open is a failed check, not a silent empty read.
QJsonObject readJsonFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        check("theme file readable", false, path);
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeJsonFile(const QString &path, const QJsonObject &object)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        check("theme file writable", false, path);
        return;
    }
    f.write(QJsonDocument(object).toJson());
}

void testTheme()
{
    begin("Theme palette");
    const QString path = ThemeManager::configFilePath();
    QFile::remove(path);

    const QPalette system = QApplication::palette();

    // First run: a starter file, but the system palette stays live.
    ThemeManager::applyTheme();
    QJsonObject root = readJsonFile(path);
    check("starter file holds no applied colors", !root.contains(QStringLiteral("colors")));
    check("starter file offers example colors", root.contains(QStringLiteral("exampleColors")));

    // A real theme must keep disabled text distinguishable.
    QJsonObject colors;
    colors[QStringLiteral("window")] = QStringLiteral("#2E3440");
    colors[QStringLiteral("button")] = QStringLiteral("#434C5E");
    colors[QStringLiteral("buttonText")] = QStringLiteral("#ECEFF4");
    colors[QStringLiteral("text")] = QStringLiteral("#E5E9F0");
    colors[QStringLiteral("base")] = QStringLiteral("#2E3440");
    root[QStringLiteral("colors")] = colors;
    writeJsonFile(path, root);
    ThemeManager::applyTheme();
    const QPalette themed = QApplication::palette();
    equal("theme color applied", themed.color(QPalette::Active, QPalette::ButtonText).name(),
          QStringLiteral("#eceff4"));
    check("disabled button text differs from active",
          themed.color(QPalette::Disabled, QPalette::ButtonText) != themed.color(QPalette::Active, QPalette::ButtonText),
          themed.color(QPalette::Disabled, QPalette::ButtonText).name());

    // A 0.3.0 file: "colors" equal to the seeded system snapshot is no
    // choice of the user's and must stop freezing the system theme.
    QJsonObject snapshot;
    snapshot[QStringLiteral("window")] = QStringLiteral("#ffefefef");
    QJsonObject old;
    old[QStringLiteral("colors")] = snapshot;
    old[QStringLiteral("systemDefaultColors")] = snapshot;
    old[QStringLiteral("defaultIconSize")] = 220;
    writeJsonFile(path, old);
    ThemeManager::applyTheme();
    root = readJsonFile(path);
    check("old seeded snapshot removed", !root.contains(QStringLiteral("colors"))
          && !root.contains(QStringLiteral("systemDefaultColors")));
    check("system palette back in use",
          QApplication::palette().color(QPalette::Disabled, QPalette::ButtonText)
              == system.color(QPalette::Disabled, QPalette::ButtonText));

    QFile::remove(path);
}

} // namespace

int main(int argc, char **argv)
{
    // Redirects settings and app data into a throwaway location.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("blairqt-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("blairqt-tests"));

    // Start from a clean database every run.
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/library.sqlite3"));

    testHtmlEntities();
    testNetworkLogRedaction();
    testBlacklist();
    testTagInput();
    testSources();
    testViewerUrls();
    testAutoLoadChain();
    testTagExclusionComposition();
    testPluginSource();
    testLibraryDatabase();
    testLibraryExactTags();
    testLibraryPathBatch();
    testLibraryMigration();
    testSourceManager();
    testThumbnailCancel();
    testDownloads();
    // Last: it changes the application palette.
    testTheme();

    std::cout << (failures ? "\nFAILED checks: " : "\nall checks passed: ") << failures << "\n";
    return failures == 0 ? 0 : 1;
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Regression tests: no network beyond this machine, no real user data.
// Whatever needs a server talks to one started inside the test, on the
// loopback interface. Everything that touches settings or the database runs
// under QStandardPaths test mode, so running this never disturbs an
// installed copy.
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
#include "sources/ShimmieSource.h"
#include "sources/GelbooruHtmlSource.h"
#include "sources/PythonPluginSource.h"
#include "NetworkLog.h"
#include "ThumbnailLoader.h"
#include "ThemeManager.h"
#include "DownloadManager.h"
#include "SourceManager.h"
#include "SourceDetector.h"
#include "SiteAccess.h"
#include "AppInfo.h"
#include "AnimatedThumbnails.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "SearchTab.h"
#include "LibraryTab.h"
#include "LibraryImport.h"
#include "FolderAccessGuard.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QInputDialog>
#include <QMimeData>
#include <QDropEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
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
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QFile>
#include <QListView>
#include <QPixmap>
#include <QSet>
#include <algorithm>
#include <functional>
#include <memory>
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

void testAppFiles()
{
    begin("Files shipped with the program are found");
    // The test binary sits in the build dir, like a development build of the
    // program: plugin_runner.py and translations/ are copied next to it.
    const QString runner = findAppFile(QStringLiteral("plugin_runner.py"));
    check("plugin_runner.py found next to the binary", QFile::exists(runner), runner);
    const QString qm = findAppFile(QStringLiteral("translations/blairqt_de.qm"));
    check("translations found next to the binary", QFile::exists(qm), qm);
    const QString missing = findAppFile(QStringLiteral("no_such_file.txt"));
    check("a missing file still yields a path to report", !missing.isEmpty() && !QFile::exists(missing), missing);
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

    check("no extension is not video", !isVideoUrl(QStringLiteral("https://x.org/data/ab/cd/0123abcd")));
    check("a dot in a folder name does not count", !isVideoUrl(QStringLiteral("https://x.org/v1.mp4/file")));

    Post video;
    video.sampleUrl = QStringLiteral("https://x.org/sample.mp4");
    video.fileUrl = QStringLiteral("https://x.org/full.webm");
    video.previewUrl = QStringLiteral("https://x.org/thumb.jpg");
    equal("video post plays its video sample", viewerMedia(video).url, video.sampleUrl);
    check("... as a video", viewerMedia(video).video);

    Post stillSample;
    stillSample.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    stillSample.fileUrl = QStringLiteral("https://x.org/full.mp4");
    equal("a still sample does not stand in for the video", viewerMedia(stillSample).url, stillSample.fileUrl);

    Post shimmieVideo;
    shimmieVideo.fileUrl = QStringLiteral("https://x.org/_images/0123abcd");
    shimmieVideo.fileExtension = QStringLiteral("webm");
    check("extension reported by the site makes a video", viewerMedia(shimmieVideo).video);

    Post ugoira;
    ugoira.sampleUrl = QStringLiteral("https://x.org/sample.webm");
    ugoira.fileUrl = QStringLiteral("https://x.org/full.zip");
    ugoira.fileExtension = QStringLiteral("zip");
    equal("an ugoira plays its video sample, not the zip", viewerMedia(ugoira).url, ugoira.sampleUrl);
    check("... as a video", viewerMedia(ugoira).video);

    Post hiddenVideo;
    hiddenVideo.previewUrl = QStringLiteral("https://x.org/thumb.jpg");
    hiddenVideo.fileExtension = QStringLiteral("webm");
    equal("a video without its original still shows the thumbnail", viewerMedia(hiddenVideo).url,
          hiddenVideo.previewUrl);
    check("... as an image", !viewerMedia(hiddenVideo).video);

    Post localVideo;
    localVideo.localFilePath = QStringLiteral("/home/u/clip.webm");
    localVideo.fileUrl = QStringLiteral("https://x.org/full.webm");
    equal("downloaded video plays from disk", viewerMedia(localVideo).url,
          QUrl::fromLocalFile(localVideo.localFilePath).toString());
    check("... as a video", viewerMedia(localVideo).video);

    Post image;
    image.localFilePath = QStringLiteral("/home/u/pic.png");
    image.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    equal("local image still wins", viewerMedia(image).url,
          QUrl::fromLocalFile(QStringLiteral("/home/u/pic.png")).toString());
    check("... as an image", !viewerMedia(image).video);

    Post remote;
    remote.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    remote.fileUrl = QStringLiteral("https://x.org/full.png");
    equal("remote viewer prefers the sample", viewerMedia(remote).url, remote.sampleUrl);
    equal("downloads still take the original", fullImageUrl(remote), remote.fileUrl);

    check("a still image does not animate", animatedThumbnailMedia(remote).url.isEmpty());
    equal("a video animates with the viewer's choice", animatedThumbnailMedia(video).url, video.sampleUrl);
    Post gif;
    gif.sampleUrl = QStringLiteral("https://x.org/sample.jpg");
    gif.fileUrl = QStringLiteral("https://x.org/full.GIF");
    equal("a GIF animates from its original", animatedThumbnailMedia(gif).url, gif.fileUrl);
    check("... not as a video", !animatedThumbnailMedia(gif).video);
    Post shimmieGif;
    shimmieGif.fileUrl = QStringLiteral("https://x.org/_images/0123abcd");
    shimmieGif.fileExtension = QStringLiteral("gif");
    equal("a GIF known by its reported extension", animatedThumbnailMedia(shimmieGif).url, shimmieGif.fileUrl);
    Post localPng;
    localPng.localFilePath = QStringLiteral("/home/u/pic.png");
    localPng.fileUrl = QStringLiteral("https://x.org/full.gif");
    check("a downloaded still stays still", animatedThumbnailMedia(localPng).url.isEmpty());
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
        } else if (firstLine.contains(" /cdn/ab/cd/abcdef ")) {
            // A Shimmie2-style CDN path: no extension in the URL.
            const QByteArray body = okBody();
            response = "HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\nContent-Length: "
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

void testShimmieSource()
{
    begin("Shimmie2 source (Danbooru-compatible API)");

    // As rule34.paheal.net answers: XML, no extension in file_url, rating "?".
    const QByteArray xml =
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<posts count='483' offset='0'>"
        "<post id='7001' md5='0123456789abcdef0123456789abcdef' file_name='0123456789abcdef0123456789abcdef.webp'"
        " file_url='https://cdn.example/01/23/0123456789abcdef0123456789abcdef' height='900' width='1200'"
        " preview_url='https://thumbs.example/01/23/0123456789abcdef0123456789abcdef'"
        " preview_height='150' preview_width='200' rating='?' date='2026-10-01 12:00:00'"
        " is_warehoused='false' tags='Cat_Noir tabby&amp;cat' source='' score='0' author='someone'/>"
        "<post id='7002' md5='x' file_name='x.png' file_url='' preview_url='' rating='s' tags='' score='3'/>"
        "</posts>";
    const ShimmieSource source(QStringLiteral("Paheal"), QStringLiteral("https://shimmie.example"));
    const QVector<Post> posts = source.parseSearchResponse(xml);
    check("a post without a file is dropped", posts.size() == 1, QString::number(posts.size()));
    if (!posts.isEmpty()) {
        const Post &p = posts.first();
        equal("id", p.id, QStringLiteral("7001"));
        equal("file url kept as sent", p.fileUrl,
              QStringLiteral("https://cdn.example/01/23/0123456789abcdef0123456789abcdef"));
        equal("extension from file_name", p.fileExtension, QStringLiteral("webp"));
        check("tags split, entities decoded",
              p.tags == QStringList({QStringLiteral("Cat_Noir"), QStringLiteral("tabby&cat")}),
              p.tags.join(QLatin1Char(' ')));
        equal("\"?\" is no rating", p.rating, QString());
        check("size", p.width == 1200 && p.height == 900);
        equal("post page works without nice URLs", p.postPageUrl,
              QStringLiteral("https://shimmie.example/index.php?q=post/view/7001"));
    }

    const QUrlQuery query(source.buildSearchRequest(QStringLiteral("cat -dog"), 3, 500).url());
    equal("API through index.php?q=", query.queryItemValue(QStringLiteral("q")),
          QStringLiteral("api/danbooru/find_posts"));
    equal("page is 1-based", query.queryItemValue(QStringLiteral("page")), QStringLiteral("3"));
    equal("limit capped at the API's 100", query.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));

    // {"tag": {"newtag": alias or null, "count": N}}; keys arrive sorted by
    // name, the result must be by use.
    const QByteArray suggestions =
        R"({"Cat_Noir":{"newtag":null,"count":5},"Catra":{"newtag":null,"count":50},)"
        R"("Catgirl":{"newtag":"cat_girl","count":20}})";
    check("suggestions by count, alias resolved",
          source.parseTagSuggestions(suggestions)
              == QStringList({QStringLiteral("Catra"), QStringLiteral("cat_girl"), QStringLiteral("Cat_Noir")}),
          source.parseTagSuggestions(suggestions).join(QLatin1Char(' ')));
    check("old plain-count suggestions",
          source.parseTagSuggestions(R"({"a":1,"b":9})") == QStringList({QStringLiteral("b"), QStringLiteral("a")}));
    check("shimmie is a known flavor", SourceManager::knownFlavors().contains(QStringLiteral("shimmie")));
    check("SourceManager creates it",
          !SourceManager::createSource(QStringLiteral("x"), QStringLiteral("shimmie"), QStringLiteral("https://a"))
               .isNull());
}

void testDownloadExtensionHint()
{
    begin("A file URL without an extension is saved with the site's one");

    QTemporaryDir dir;
    TestHttpServer server;
    DownloadManager manager(nullptr);
    manager.setDownloadDir(dir.path());

    Post post;
    post.id = QStringLiteral("7001");
    post.sourceName = QStringLiteral("Paheal");
    post.fileUrl = server.url(QStringLiteral("/cdn/ab/cd/abcdef"));
    post.fileExtension = QStringLiteral("webp");

    QString savedPath;
    bool finished = false;
    QObject::connect(&manager, &DownloadManager::downloadFinished,
                     [&](const QString &path, bool, bool, const QString &) {
                         savedPath = path;
                         finished = true;
                     });
    manager.download(post);
    QElapsedTimer waited;
    waited.start();
    while (!finished && waited.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check("saved as .webp", savedPath.endsWith(QStringLiteral("/Paheal_7001.webp")), savedPath);

    post.id = QStringLiteral("7002");
    post.fileExtension = QStringLiteral("../x");
    finished = false;
    manager.download(post);
    waited.restart();
    while (!finished && waited.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check("a hostile extension stays inside the folder",
          !savedPath.isEmpty() && QFileInfo(savedPath).absolutePath() == QDir(dir.path()).absolutePath(),
          savedPath);
}

void testDetectorAddresses()
{
    begin("Source detection: what the user typed becomes a base address");

    struct Case {
        QString typed;
        QStringList want;
    };
    const QVector<Case> cases = {
        {QStringLiteral("safebooru.org"), {QStringLiteral("https://safebooru.org")}},
        {QStringLiteral("  https://safebooru.org/  "), {QStringLiteral("https://safebooru.org")}},
        {QStringLiteral("https://safebooru.org/index.php?page=post&s=view&id=5"), {QStringLiteral("https://safebooru.org")}},
        {QStringLiteral("https://danbooru.donmai.us/posts/123?q=cat"), {QStringLiteral("https://danbooru.donmai.us")}},
        {QStringLiteral("https://yande.re/post/show/12"), {QStringLiteral("https://yande.re")}},
        {QStringLiteral("https://derpibooru.org/images/5"), {QStringLiteral("https://derpibooru.org")}},
        {QStringLiteral("https://rule34.paheal.net/post/view/1"), {QStringLiteral("https://rule34.paheal.net")}},
        {QStringLiteral("https://szuru.example/#/posts"), {QStringLiteral("https://szuru.example")}},
        {QStringLiteral("HTTPS://Example.COM/Posts?tags=x"), {QStringLiteral("https://example.com")}},
        {QStringLiteral("http://example.com:8080/booru/index.php?page=post&s=list"),
         {QStringLiteral("http://example.com:8080/booru"), QStringLiteral("http://example.com:8080")}},
        {QStringLiteral("https://example.com/booru/"),
         {QStringLiteral("https://example.com/booru"), QStringLiteral("https://example.com")}},
        {QStringLiteral("ftp://example.com"), {}},
        {QString(), {}},
        {QStringLiteral("not an address"), {}},
    };
    for (const Case &c : cases) {
        const QStringList got = SourceDetector::candidateBaseUrls(c.typed);
        check(qPrintable(QStringLiteral("\"%1\"").arg(c.typed)), got == c.want, got.join(QStringLiteral(" | ")));
    }
}

void testDetectorClues()
{
    begin("Source detection: what a front page gives away");

    struct Case {
        const char *name;
        QByteArray html;
        QString firstFlavor;
        QString blocker;
        bool gelbooruListing;
    };
    const QVector<Case> cases = {
        {"Gelbooru links", R"(<a href="index.php?page=post&amp;s=list&amp;tags=all">Posts</a>)",
         QStringLiteral("gelbooru"), QString(), true},
        {"Danbooru links", R"(<a href="/posts?tags=cat">cat</a> <a href="/posts/12">x</a>)",
         QStringLiteral("danbooru"), QString(), false},
        {"e621 before Danbooru", R"(<title>e621</title><a href="/posts/5">x</a>)",
         QStringLiteral("e621"), QString(), false},
        {"Moebooru links", R"(<a href="/post/show/1271004">x</a>)", QStringLiteral("moebooru"), QString(), false},
        {"Shimmie2 footer", R"(<footer>Running <a href="https://github.com/shish/shimmie2">Shimmie2</a></footer>)",
         QStringLiteral("shimmie"), QString(), false},
        {"Philomena", R"(<div class="image-container" data-image-id="5">)", QStringLiteral("philomena"),
         QString(), false},
        {"Cloudflare", "<html><head><title>Just a moment...</title></head><body>cf-chl</body></html>",
         QString(), QStringLiteral("Cloudflare"), false},
        {"Anubis", R"(<title>Oh noes!</title><img src="/.within.website/x/cmd/anubis/static/img/reject.webp">)",
         QString(), QStringLiteral("Anubis"), false},
        {"an ordinary page", "<html><body>Hello</body></html>", QString(), QString(), false},
    };
    for (const Case &c : cases) {
        const SourceDetector::PageClues clues = SourceDetector::readFrontPage(c.html);
        check(c.name, clues.flavors.value(0) == c.firstFlavor && clues.blocker == c.blocker
                          && clues.gelbooruListing == c.gelbooruListing,
              clues.flavors.join(QLatin1Char(',')) + QLatin1Char('/') + clues.blocker);
    }

    check("Rule34's missing key", SourceDetector::asksForCredentials(
        200, "Missing authentication. Go to api.rule34.xxx for more information"));
    check("HTTP 401", SourceDetector::asksForCredentials(401, ""));
    check("an empty answer is no request for a key", !SourceDetector::asksForCredentials(200, "[]"));
}

// Answers each request through a function of path and query, and keeps the
// paths it was asked for.
class RouteServer : public QTcpServer {
public:
    struct Answer {
        int status = 404;
        QByteArray type = "text/html";
        QByteArray body;
        QByteArray location;
        // Sent this long after the request arrived.
        int delayMs = 0;
    };
    using Handler = std::function<Answer(const QString &path, const QUrlQuery &query)>;

    explicit RouteServer(Handler handler) : m_handler(std::move(handler))
    {
        acceptFrom(this);
        listen(QHostAddress::LocalHost, 0);
        // The same port on the IPv6 loopback too, where the machine has one.
        // "localhost" is ::1 first on macOS, and a connection to a port
        // nobody listens on is not refused there but left to time out.
        acceptFrom(&m_ipv6);
        m_ipv6.listen(QHostAddress::LocalHostIPv6, serverPort());
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

    QStringList requested;
    // Every request as it arrived, headers included.
    QList<QByteArray> rawRequests;
    // Requests being answered at one moment, and the most there ever were.
    int openRequests = 0;
    int mostOpenRequests = 0;

    // The Cookie header of each request for `path`, "-" where there was none.
    QStringList cookiesSentTo(const QString &path) const
    {
        QStringList cookies;
        for (const QByteArray &raw : rawRequests) {
            const QByteArray target = raw.left(raw.indexOf("\r\n")).split(' ').value(1);
            if (QUrl(base() + QString::fromUtf8(target)).path() != path)
                continue;
            QString cookie = QStringLiteral("-");
            for (const QByteArray &line : raw.split('\n')) {
                if (line.toLower().startsWith("cookie:"))
                    cookie = QString::fromUtf8(line.mid(7).trimmed());
            }
            cookies.append(cookie);
        }
        return cookies;
    }

private:
    void acceptFrom(QTcpServer *server)
    {
        connect(server, &QTcpServer::newConnection, this, [this, server]() {
            while (server->hasPendingConnections()) {
                QTcpSocket *socket = server->nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() { onReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void onReadyRead(QTcpSocket *socket)
    {
        const QByteArray buffered = socket->property("buffered").toByteArray() + socket->readAll();
        socket->setProperty("buffered", buffered);
        if (!buffered.contains("\r\n\r\n"))
            return;
        socket->setProperty("buffered", QByteArray());

        rawRequests.append(buffered);
        const QByteArray target = buffered.left(buffered.indexOf("\r\n")).split(' ').value(1);
        const QUrl url(base() + QString::fromUtf8(target));
        requested.append(url.path() + (url.hasQuery() ? QLatin1Char('?') + url.query() : QString()));
        const Answer answer = m_handler(url.path(), QUrlQuery(url));

        QByteArray response = "HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\nContent-Type: "
            + answer.type + "\r\nContent-Length: " + QByteArray::number(answer.body.size()) + "\r\n";
        if (!answer.location.isEmpty())
            response += "Location: " + answer.location + "\r\n";
        response += "Connection: close\r\n\r\n" + answer.body;

        ++openRequests;
        mostOpenRequests = qMax(mostOpenRequests, openRequests);
        const auto send = [this, socket, response]() {
            --openRequests;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        };
        if (answer.delayMs > 0)
            QTimer::singleShot(answer.delayMs, socket, send);
        else
            send();
    }

    Handler m_handler;
    QTcpServer m_ipv6;
};

// A site that is down: it takes the connection and drops it at once.
// Not a closed port, which some systems (the macOS CI runners) do not
// refuse but leave to time out.
class DroppingServer : public QTcpServer {
public:
    DroppingServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections())
                nextPendingConnection()->abort();
        });
        listen(QHostAddress::LocalHost, 0);
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
};

// Runs one detection to its end.
struct Detection {
    SourceDetector::Result result;
    QStringList probed; // "flavor:tag" in order
    bool done = false;
};

Detection detect(const QString &url, const QString &flavor = {})
{
    Detection d;
    SourceDetector detector;
    QObject::connect(&detector, &SourceDetector::probing,
                     [&d](const QString &f, int, int, const QString &tag) { d.probed.append(f + QLatin1Char(':') + tag); });
    QObject::connect(&detector, &SourceDetector::finished, [&d](const SourceDetector::Result &r) {
        d.result = r;
        d.done = true;
    });
    SourceDetector::Request request;
    request.url = url;
    request.flavor = flavor;
    detector.setRequestSpacing(0);
    detector.start(request);
    QElapsedTimer waited;
    waited.start();
    while (!d.done && waited.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return d;
}

RouteServer::Answer html(const QByteArray &body, int status = 200)
{
    RouteServer::Answer a;
    a.status = status;
    a.body = body;
    return a;
}

RouteServer::Answer data(const QByteArray &body, const QByteArray &type = "application/json")
{
    RouteServer::Answer a;
    a.status = 200;
    a.type = type;
    a.body = body;
    return a;
}

// Three 8x8 frames - red, green, blue - 40 ms each, looping.
const char kTinyGif[] =
    "\x47\x49\x46\x38\x39\x61\x08\x00\x08\x00\x81\x00\x00\xff\x00\x00\x00\xff"
    "\x00\x00\x00\xff\x00\x00\x00\x21\xff\x0b\x4e\x45\x54\x53\x43\x41\x50\x45"
    "\x32\x2e\x30\x03\x01\x00\x00\x00\x21\xf9\x04\x04\x04\x00\x00\x00\x2c\x00"
    "\x00\x00\x00\x08\x00\x08\x00\x00\x08\x0f\x00\x01\x08\x1c\x48\xb0\xa0\xc1"
    "\x83\x08\x13\x2a\x4c\x18\x10\x00\x21\xf9\x04\x04\x04\x00\x00\x00\x2c\x00"
    "\x00\x00\x00\x08\x00\x08\x00\x81\xff\x00\x00\x00\xff\x00\x00\x00\xff\x00"
    "\x00\x00\x08\x0f\x00\x03\x08\x1c\x48\xb0\xa0\xc1\x83\x08\x13\x2a\x4c\x18"
    "\x10\x00\x21\xf9\x04\x04\x04\x00\x00\x00\x2c\x00\x00\x00\x00\x08\x00\x08"
    "\x00\x81\xff\x00\x00\x00\xff\x00\x00\x00\xff\x00\x00\x00\x08\x0f\x00\x05"
    "\x08\x1c\x48\xb0\xa0\xc1\x83\x08\x13\x2a\x4c\x18\x10\x00\x3b";

// The center pixel of a row's thumbnail, without alpha; 0 when it has none.
QRgb thumbnailCenter(const GalleryModel &model, int row)
{
    const QPixmap pixmap = model.thumbnailAt(row);
    if (pixmap.isNull())
        return 0;
    const QImage image = pixmap.toImage();
    return image.pixel(image.width() / 2, image.height() / 2) & 0x00ffffff;
}

void testAnimatedThumbnails()
{
    begin("Animated thumbnails (GIF in the grid)");

    const QByteArray gif(kTinyGif, sizeof(kTinyGif) - 1);
    RouteServer server([gif](const QString &path, const QUrlQuery &) {
        if (path.startsWith(QStringLiteral("/gif/")))
            return data(gif, "image/gif");
        return html("missing", 404);
    });

    QTemporaryDir dir;
    const QString localGif = dir.filePath(QStringLiteral("local.gif"));
    {
        QFile file(localGif);
        file.open(QIODevice::WriteOnly);
        file.write(gif);
    }

    // 0: a remote GIF. 1: a still image. 2: a GIF that is not there.
    // 3: a downloaded GIF. 4 to 11: more remote GIFs, past the cap.
    QVector<Post> posts;
    for (int row = 0; row < 12; ++row) {
        Post post;
        post.id = QString::number(row);
        post.fileUrl = server.base() + QStringLiteral("/gif/%1.gif").arg(row);
        posts.append(post);
    }
    posts[1].fileUrl = server.base() + QStringLiteral("/still.png");
    posts[2].fileUrl = server.base() + QStringLiteral("/gone.gif");
    posts[3].fileUrl.clear();
    posts[3].localFilePath = localGif;

    GalleryModel model;
    model.appendPosts(posts);
    QListView view;
    GalleryView::configure(&view);
    view.setModel(&model);
    GalleryView::setCellSize(&view, QSize(40, 40));
    view.resize(400, 400);
    view.show();

    QSet<QRgb> remoteColors;
    QSet<QRgb> localColors;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&](const QModelIndex &topLeft) {
        if (topLeft.row() == 0)
            remoteColors.insert(thumbnailCenter(model, 0));
        else if (topLeft.row() == 3)
            localColors.insert(thumbnailCenter(model, 3));
    });

    AnimatedThumbnails animations(&view, &model);
    animations.setSettleDelay(0);
    auto spin = [](int milliseconds, const std::function<bool()> &done) {
        QElapsedTimer waited;
        waited.start();
        while (!done() && waited.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };

    animations.refresh();
    spin(200, []() { return false; });
    check("off until enabled", animations.playingRows().isEmpty() && remoteColors.isEmpty());

    animations.setEnabled(true);
    spin(10000, [&]() { return remoteColors.size() >= 3 && localColors.size() >= 3; });
    check("a remote GIF shows all its frames", remoteColors.size() >= 3, QString::number(remoteColors.size()));
    check("a downloaded GIF too", localColors.size() >= 3, QString::number(localColors.size()));

    QList<int> playing = animations.playingRows();
    std::sort(playing.begin(), playing.end());
    QStringList playingText;
    for (const int row : std::as_const(playing))
        playingText.append(QString::number(row));
    check("the still image stays still", !playing.contains(1), playingText.join(QLatin1Char(' ')));
    check("the missing GIF gives up its place", !playing.contains(2), playingText.join(QLatin1Char(' ')));
    equal("as many as the cap, in grid order", playingText.join(QLatin1Char(' ')), QStringLiteral("0 3 4 5 6 7"));

    const QSize cell = GalleryView::thumbnailSizeForCell(QSize(40, 40));
    check("frames are scaled to the cell", model.thumbnailAt(0).size() == cell,
          QStringLiteral("%1x%2").arg(model.thumbnailAt(0).width()).arg(model.thumbnailAt(0).height()));

    view.hide();
    QCoreApplication::processEvents();
    check("a hidden grid stops", animations.playingRows().isEmpty());
    view.show();
    spin(5000, [&]() { return animations.playingRows().size() == AnimatedThumbnails::kMaxPlaying; });
    check("shown again, it plays again", animations.playingRows().size() == AnimatedThumbnails::kMaxPlaying);

    animations.setEnabled(false);
    check("turned off, nothing plays", animations.playingRows().isEmpty());
    remoteColors.clear();
    spin(300, []() { return false; });
    check("and no frame arrives after", remoteColors.isEmpty());

    animations.setEnabled(true);
    spin(5000, [&]() { return !animations.playingRows().isEmpty(); });
    model.clear();
    check("new results stop the old rows", animations.playingRows().isEmpty());
}

// A booru.org post list, as steambanners.booru.org sends it (shortened):
// a post linked twice, tags in the title, a png thumbnail.
const QByteArray kBooruOrgListing = R"(
<div class="content"><span class="thumb"><a id="p34141" href="index.php?page=post&amp;s=view&amp;id=34141">
<img src="https://thumbs.booru.org/steambanners/thumbnails//5/thumbnail_01ca93e6c40f95ce6b824bafce53667211be6609.jpg"
 alt="post" border="0" title=" big sonic fan&amp;game  score:3 rating:Safe" class="preview"/></a></span>
<span class="thumb"><a id="p34141b" href="index.php?page=post&amp;s=view&amp;id=34141"><img src="https://thumbs.booru.org/steambanners/thumbnails//5/thumbnail_01ca93e6c40f95ce6b824bafce53667211be6609.jpg" title="dup"/></a></span>
<span class="thumb"><a href='index.php?page=post&amp;s=view&amp;id=34140'><img src='https://thumbs.booru.org/steambanners/thumbnails//30/thumbnail_727fd9.png' title=' cat  score:0 rating:Questionable'/></a></span>
<a href="index.php?page=post&amp;s=list&amp;pid=20">Next</a></div>)";

const QByteArray kGelbooruPosts =
    R"([{"id":3,"file_url":"https://img.example/3.jpg","preview_url":"https://img.example/t3.jpg","tags":"a b"}])";

void testDetectorEndToEnd()
{
    begin("Source detection against a local server");

    {
        // A Gelbooru-engine site installed in /booru, a post page pasted.
        RouteServer server([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/booru/"))
                return html(R"(<a href="index.php?page=post&amp;s=list">Posts</a>)");
            if (path == QStringLiteral("/booru/index.php") && query.queryItemValue(QStringLiteral("page")) == QStringLiteral("dapi"))
                return data(kGelbooruPosts);
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base() + QStringLiteral("/booru/index.php?page=post&s=view&id=3"));
        check("found", d.result.found, d.probed.join(QLatin1Char(' ')));
        equal("as gelbooru", d.result.flavor, QStringLiteral("gelbooru"));
        equal("at its folder, not the post", d.result.baseUrl, server.base() + QStringLiteral("/booru"));
        check("the front page put gelbooru first", d.probed.value(0) == QStringLiteral("gelbooru:"),
              d.probed.join(QLatin1Char(' ')));
    }

    {
        // Shimmie2, recognized by its footer and probed first.
        RouteServer server([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/"))
                return html("<footer>Running Shimmie2</footer>");
            if (path == QStringLiteral("/index.php")
                && query.queryItemValue(QStringLiteral("q")) == QStringLiteral("api/danbooru/find_posts")) {
                return data("<posts count='1' offset='0'><post id='9' file_url='https://cdn.example/9' "
                            "file_name='9.png' preview_url='https://cdn.example/t9' tags='x'/></posts>",
                            "application/xml");
            }
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base());
        equal("shimmie", d.result.flavor, QStringLiteral("shimmie"));
        check("with one probe", d.probed == QStringList{QStringLiteral("shimmie:")}, d.probed.join(QLatin1Char(' ')));
    }

    {
        // Nothing for an empty search, posts for "all": only the engine
        // that answered in its own format is asked again.
        RouteServer server([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/index.php") && query.queryItemValue(QStringLiteral("page")) == QStringLiteral("dapi")) {
                return data(query.queryItemValue(QStringLiteral("tags")) == QStringLiteral("all") ? kGelbooruPosts : QByteArray("[]"));
            }
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base());
        equal("found with a tag", d.result.flavor, QStringLiteral("gelbooru"));
        check("the retry asked only gelbooru", d.probed.last() == QStringLiteral("gelbooru:all")
                  && d.probed.count(QStringLiteral("danbooru:all")) == 0,
              d.probed.join(QLatin1Char(' ')));
    }

    {
        // Behind Cloudflare: every page is the challenge.
        RouteServer server([](const QString &, const QUrlQuery &) {
            return html("<html><head><title>Just a moment...</title></head><body>cf-chl</body></html>", 403);
        });
        const Detection d = detect(server.base());
        check("not found", !d.result.found);
        check("reported as blocked", d.result.failure == SourceDetector::Failure::Blocked);
        equal("by Cloudflare", d.result.blocker, QStringLiteral("Cloudflare"));
    }

    {
        // A Gelbooru-engine site that wants an API key.
        RouteServer server([](const QString &path, const QUrlQuery &) {
            if (path == QStringLiteral("/"))
                return html(R"(<a href="index.php?page=post&amp;s=list">Posts</a>)");
            if (path == QStringLiteral("/index.php"))
                return data("Missing authentication. Go to api.example for more information", "text/plain");
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base());
        check("asks for a key", d.result.failure == SourceDetector::Failure::NeedsCredentials);
        equal("on the gelbooru API", d.result.credentialsFlavor, QStringLiteral("gelbooru"));
    }

    {
        // A Gelbooru-engine site with its API turned off.
        RouteServer server([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/") || query.queryItemValue(QStringLiteral("page")) == QStringLiteral("dapi"))
                return html(R"(<a href="index.php?page=post&amp;s=view&amp;id=1"><img src="t.jpg"></a>)");
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base());
        check("suggests the HTML bridge", d.result.failure == SourceDetector::Failure::NeedsHtmlBridge);
    }

    {
        // The front page redirects to another address (http -> https, or
        // to www.); the probes and the saved base follow it.
        RouteServer target([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/"))
                return html(R"(<a href="index.php?page=post&amp;s=list">Posts</a>)");
            if (query.queryItemValue(QStringLiteral("page")) == QStringLiteral("dapi"))
                return data(kGelbooruPosts);
            return RouteServer::Answer();
        });
        const QByteArray location = target.base().toUtf8() + "/";
        RouteServer old([location](const QString &, const QUrlQuery &) {
            RouteServer::Answer a;
            a.status = 301;
            a.location = location;
            return a;
        });
        const Detection d = detect(old.base());
        check("found", d.result.found);
        equal("at the new address", d.result.baseUrl, target.base());
    }

    {
        // One engine chosen: only it is asked, the front page is not read.
        RouteServer server([](const QString &, const QUrlQuery &) { return data("[]"); });
        const Detection d = detect(server.base(), QStringLiteral("moebooru"));
        check("only moebooru", d.probed.value(0) == QStringLiteral("moebooru:")
                  && !d.probed.join(QLatin1Char(' ')).contains(QStringLiteral("danbooru")),
              d.probed.join(QLatin1Char(' ')));
        check("no front page request", !server.requested.contains(QStringLiteral("/")),
              server.requested.join(QLatin1Char(' ')));
    }

    {
        const Detection d = detect(QStringLiteral("ftp://nope"));
        check("not a web address", d.result.failure == SourceDetector::Failure::InvalidUrl);
    }

    {
        // booru.org: Gelbooru 0.1, no API; its HTML list is read instead.
        RouteServer server([](const QString &path, const QUrlQuery &query) {
            if (path == QStringLiteral("/"))
                return html(R"(<a href="index.php?page=post&amp;s=list">Posts</a>)");
            if (path == QStringLiteral("/index.php") && query.queryItemValue(QStringLiteral("s")) == QStringLiteral("list"))
                return html(kBooruOrgListing);
            if (path == QStringLiteral("/index.php"))
                return html("<html><body>Gelbooru front page</body></html>");
            return RouteServer::Answer();
        });
        const Detection d = detect(server.base());
        equal("booru.org board read as gelbooru-html", d.result.flavor, QStringLiteral("gelbooru-html"));
        check("right after the API that is not there",
              d.probed == QStringList({QStringLiteral("gelbooru:"), QStringLiteral("gelbooru-html:")}),
              d.probed.join(QLatin1Char(' ')));
    }

    {
        // 429: stop at once instead of asking every other engine.
        RouteServer server([](const QString &path, const QUrlQuery &) {
            if (path == QStringLiteral("/"))
                return html(R"(<a href="index.php?page=post&amp;s=list">Posts</a>)");
            return html("<html><title>429 Too Many Requests</title></html>", 429);
        });
        const Detection d = detect(server.base());
        check("reported as rate limited", d.result.failure == SourceDetector::Failure::RateLimited);
        check("no request after the 429", server.requested.size() == 2, server.requested.join(QLatin1Char(' ')));
    }

    {
        // Requests are spaced out by default.
        RouteServer server([](const QString &, const QUrlQuery &) { return data("[]"); });
        SourceDetector detector;
        detector.setRequestSpacing(150);
        bool done = false;
        QObject::connect(&detector, &SourceDetector::finished, [&done](const SourceDetector::Result &) { done = true; });
        SourceDetector::Request request;
        request.url = server.base();
        request.flavor = QStringLiteral("danbooru");
        QElapsedTimer timer;
        timer.start();
        detector.start(request);
        while (!done && timer.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        // One engine, three passes ("", "all", "1girl"): three spaced requests.
        check("three probes take at least three spacings", timer.elapsed() >= 3 * 150,
              QString::number(timer.elapsed()) + QStringLiteral(" ms"));
    }
}

void testGelbooruHtmlSource()
{
    begin("Gelbooru 0.1 HTML list (booru.org)");

    const GelbooruHtmlSource source(QStringLiteral("Banners"), QStringLiteral("https://steambanners.booru.org"));
    const QVector<Post> posts = source.parseSearchResponse(kBooruOrgListing);
    check("two posts, the repeated link once", posts.size() == 2, QString::number(posts.size()));
    if (posts.size() == 2) {
        const Post &p = posts.first();
        equal("id", p.id, QStringLiteral("34141"));
        equal("thumbnail", p.previewUrl, QStringLiteral(
            "https://thumbs.booru.org/steambanners/thumbnails//5/thumbnail_01ca93e6c40f95ce6b824bafce53667211be6609.jpg"));
        equal("image derived from it", p.fileUrl, QStringLiteral(
            "https://img.booru.org/steambanners//images/5/01ca93e6c40f95ce6b824bafce53667211be6609.jpg"));
        check("tags from the title", p.tags == QStringList({QStringLiteral("big"), QStringLiteral("sonic"),
                                                            QStringLiteral("fan&game")}),
              p.tags.join(QLatin1Char(' ')));
        equal("rating", p.rating, QStringLiteral("s"));
        check("score", p.score == 3);
        equal("post page", p.postPageUrl,
              QStringLiteral("https://steambanners.booru.org/index.php?page=post&s=view&id=34141"));
        equal("a png thumbnail keeps .png", posts.at(1).fileUrl,
              QStringLiteral("https://img.booru.org/steambanners//images/30/727fd9.png"));
    }

    // Self-hosted: relative thumbnail, single slashes.
    const GelbooruHtmlSource selfHosted(QStringLiteral("Own"), QStringLiteral("https://own.example/booru"));
    const QVector<Post> own = selfHosted.parseSearchResponse(
        R"(<span class="thumb"><a href="index.php?page=post&s=view&id=7"><img src="thumbnails/12/thumbnail_abc.gif" title="x rating:Explicit"></a></span>)");
    check("relative thumbnail made absolute", own.size() == 1
              && own.first().previewUrl == QStringLiteral("https://own.example/booru/thumbnails/12/thumbnail_abc.gif"),
          own.isEmpty() ? QString() : own.first().previewUrl);
    check("self-hosted image address", !own.isEmpty()
              && own.first().fileUrl == QStringLiteral("https://own.example/booru/images/12/abc.gif"),
          own.isEmpty() ? QString() : own.first().fileUrl);
    check("an unknown thumbnail layout gives no file", GelbooruHtmlSource::imageUrlForThumbnail(
        QStringLiteral("https://cdn.example/t/abc.jpg")).isEmpty());

    const QUrlQuery first(source.buildSearchRequest(QString(), 1, 40).url());
    const QUrlQuery third(source.buildSearchRequest(QStringLiteral("cat"), 3, 40).url());
    equal("empty search lists all", first.queryItemValue(QStringLiteral("tags")), QStringLiteral("all"));
    equal("page 1 at offset 0", first.queryItemValue(QStringLiteral("pid")), QStringLiteral("0"));
    equal("page 3 at offset 40", third.queryItemValue(QStringLiteral("pid")), QStringLiteral("40"));
}

void testSiteAccess()
{
    begin("A pasted cookie and User-Agent go with every request to that site");

    QHash<QString, SiteAccess::Credentials> byHost;
    byHost.insert(QStringLiteral("www.protected.example"), {QStringLiteral("cf_clearance=abc"), QStringLiteral("Mozilla/5.0 Test")});
    SiteAccess::setAll(byHost);

    const auto sent = [](const QString &url) {
        QNetworkRequest request{QUrl(url)};
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("blairqt"));
        SiteAccess::apply(request);
        return QString::fromUtf8(request.rawHeader("Cookie")) + QLatin1Char('|')
            + request.header(QNetworkRequest::UserAgentHeader).toString();
    };
    equal("the site", sent(QStringLiteral("https://protected.example/posts.json")),
          QStringLiteral("cf_clearance=abc|Mozilla/5.0 Test"));
    equal("its www. form", sent(QStringLiteral("https://www.protected.example/")),
          QStringLiteral("cf_clearance=abc|Mozilla/5.0 Test"));
    equal("a subdomain (thumbnails, images)", sent(QStringLiteral("https://cdn.protected.example/t/1.jpg")),
          QStringLiteral("cf_clearance=abc|Mozilla/5.0 Test"));
    equal("another site gets nothing", sent(QStringLiteral("https://safebooru.org/")), QStringLiteral("|blairqt"));
    equal("not by the top-level domain", sent(QStringLiteral("https://other.org/")), QStringLiteral("|blairqt"));

    SiteAccess::setAll({});
    equal("cleared", sent(QStringLiteral("https://protected.example/")), QStringLiteral("|blairqt"));
}


// Runs the event loop until done() says so or the time is up.
bool waitFor(int milliseconds, const std::function<bool()> &done)
{
    QElapsedTimer waited;
    waited.start();
    while (!done() && waited.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// Runs the event loop for a fixed time.
void spinFor(int milliseconds)
{
    waitFor(milliseconds, []() { return false; });
}

// A query item's value as a web server reads it off the wire: "+" is a
// space, then the percent escapes are resolved.
QString asServerReads(const QUrl &url, const QByteArray &key)
{
    const QByteArray wire = url.toEncoded();
    const QList<QByteArray> pairs = wire.mid(wire.indexOf('?') + 1).split('&');
    for (const QByteArray &pair : pairs) {
        const int equals = pair.indexOf('=');
        if (equals < 0 || QByteArray::fromPercentEncoding(pair.left(equals)) != key)
            continue;
        QByteArray value = pair.mid(equals + 1);
        value.replace('+', ' ');
        return QString::fromUtf8(QByteArray::fromPercentEncoding(value));
    }
    return QStringLiteral("<no such item>");
}

void testQueryEncoding()
{
    begin("Search terms reach the server as they were typed");

    // "6+girls" used to arrive as "6 girls" and find nothing; a literal
    // "%2B" used to arrive as "+".
    const QString tags = QStringLiteral("6+girls c++ 100%_done a&b rating:safe -tag_(x) lit%2Beral a=b #1");
    const QString site = QStringLiteral("https://site.example");

    const QList<QPair<QString, QByteArray>> flavors = {
        {QStringLiteral("gelbooru"), "tags"},   {QStringLiteral("danbooru"), "tags"},
        {QStringLiteral("moebooru"), "tags"},   {QStringLiteral("e621"), "tags"},
        {QStringLiteral("szurubooru"), "query"}, {QStringLiteral("shimmie"), "tags"},
        {QStringLiteral("gelbooru-html"), "tags"},
    };
    for (const auto &[flavor, key] : flavors) {
        const auto source = SourceManager::createSource(QStringLiteral("S"), flavor, site);
        const QUrl url = source->buildSearchRequest(tags, 1, 5).url();
        equal(qPrintable(flavor + QStringLiteral(": search")), asServerReads(url, key), tags);
    }

    // Philomena separates terms with ", " and writes "_" as a space.
    PhilomenaSource philomena(QStringLiteral("P"), site);
    equal("philomena: search", asServerReads(philomena.buildSearchRequest(tags, 1, 5).url(), "q"),
          QStringLiteral("6+girls, c++, 100% done, a&b, rating:safe, -tag (x), lit%2Beral, a=b, #1"));

    GelbooruSource bridged(QStringLiteral("B"), QStringLiteral("https://site.example/a+b"), {}, {}, {}, {},
                           QStringLiteral("http://127.0.0.1:8765"));
    const QUrl bridgeUrl = bridged.buildSearchRequest(tags, 1, 5).url();
    equal("scrape bridge: search", asServerReads(bridgeUrl, "tags"), tags);
    equal("scrape bridge: site address", asServerReads(bridgeUrl, "base_url"), QStringLiteral("https://site.example/a+b"));

    GelbooruSource keyed(QStringLiteral("K"), site, QStringLiteral("k+e/y%3D"), QStringLiteral("u+1"));
    const QUrl keyedUrl = keyed.buildSearchRequest(QStringLiteral("x"), 1, 5).url();
    equal("API key", asServerReads(keyedUrl, "api_key"), QStringLiteral("k+e/y%3D"));
    equal("user id", asServerReads(keyedUrl, "user_id"), QStringLiteral("u+1"));

    const QString prefix = QStringLiteral("c++");
    const QList<QPair<QString, QByteArray>> suggesting = {
        {QStringLiteral("gelbooru"), "q"},  {QStringLiteral("danbooru"), "search[query]"},
        {QStringLiteral("moebooru"), "name"}, {QStringLiteral("e621"), "search[name_matches]"},
        {QStringLiteral("shimmie"), "s"},
    };
    for (const auto &[flavor, key] : suggesting) {
        const auto source = SourceManager::createSource(QStringLiteral("S"), flavor, site);
        equal(qPrintable(flavor + QStringLiteral(": tag suggestions")),
              asServerReads(source->buildTagSuggestRequest(prefix).url(), key), prefix);
    }
    equal("philomena: tag suggestions", asServerReads(philomena.buildTagSuggestRequest(prefix).url(), "q"),
          QStringLiteral("c++*"));

    // The readable characters of tag syntax stay readable in the log.
    const QString plain = QString::fromLatin1(
        SourceManager::createSource(QStringLiteral("S"), QStringLiteral("moebooru"), site)
            ->buildSearchRequest(QStringLiteral("rating:safe tag_(x)"), 1, 5).url().toEncoded());
    check("colon and parentheses are not escaped", plain.contains(QStringLiteral("tags=rating:safe%20tag_(x)")), plain);
}

void testTagExclusionLimits()
{
    begin("The blacklist never makes a search the site rejects");

    QStringList sixty;
    for (int i = 0; i < 60; ++i)
        sixty << QStringLiteral("bad_%1").arg(i);

    // e621 answers HTTP 422 to the 41st term.
    E621Source e621(QStringLiteral("e621"), QStringLiteral("https://e621.net"));
    check("e621 has a term limit", e621.maxSearchTerms() == 40);
    const QStringList composed = searchTagsWithExclusions(e621, QStringLiteral("1girl solo"), sixty)
                                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    check("terms stop at the site's limit", composed.size() == 40, QString::number(composed.size()));
    equal("the user's own terms come first", composed.mid(0, 3).join(QLatin1Char(' ')),
          QStringLiteral("1girl solo -bad_0"));
    equal("exclusions fill the rest in order", composed.last(), QStringLiteral("-bad_37"));

    QStringList own;
    for (int i = 0; i < 45; ++i)
        own << QStringLiteral("t%1").arg(i);
    equal("a query already past the limit gets nothing added",
          searchTagsWithExclusions(e621, own.join(QLatin1Char(' ')), sixty), own.join(QLatin1Char(' ')));

    // No term limit, but an address cannot grow without end either.
    MoebooruSource moebooru(QStringLiteral("yande.re"), QStringLiteral("https://yande.re"));
    check("moebooru has no term limit", moebooru.maxSearchTerms() == 0);
    equal("sixty exclusions fit", QString::number(
              searchTagsWithExclusions(moebooru, QStringLiteral("1girl"), sixty).count(QStringLiteral(" -"))),
          QStringLiteral("60"));
    QStringList thousand;
    for (int i = 0; i < 1000; ++i)
        thousand << QStringLiteral("blocked_tag_number_%1").arg(i);
    const QString longest = searchTagsWithExclusions(moebooru, QStringLiteral("1girl"), thousand);
    check("a huge blacklist is cut to a bounded length", longest.size() <= kMaxSearchTagsLength
              && longest.size() > kMaxSearchTagsLength - 40, QString::number(longest.size()));
    const QString lastTerm = longest.section(QLatin1Char(' '), -1);
    check("the cut falls between terms, not inside one",
          lastTerm.startsWith(QLatin1Char('-')) && thousand.contains(lastTerm.mid(1)), lastTerm);
}

void testClipboardImage()
{
    begin("\"Copy image\" is offered only where there is an image");
    Post image;
    image.fileUrl = QStringLiteral("https://x.org/full.png");
    equal("an image post", clipboardImageUrl(image), image.fileUrl);

    Post video;
    video.fileUrl = QStringLiteral("https://x.org/clip.webm");
    video.previewUrl = QStringLiteral("https://x.org/thumb.jpg");
    check("a video post", clipboardImageUrl(video).isEmpty());

    // Shimmie2 CDNs serve "/ab/cd/<md5>": the address says nothing.
    Post shimmieVideo;
    shimmieVideo.fileUrl = QStringLiteral("https://cdn.x.org/ab/cd/abcdef");
    shimmieVideo.fileExtension = QStringLiteral("mp4");
    check("a video whose address has no extension", clipboardImageUrl(shimmieVideo).isEmpty());
    Post shimmieImage = shimmieVideo;
    shimmieImage.fileExtension = QStringLiteral("png");
    equal("an image whose address has no extension", clipboardImageUrl(shimmieImage), shimmieImage.fileUrl);

    Post localVideo;
    localVideo.localFilePath = QStringLiteral("/home/u/clip.mp4");
    localVideo.fileUrl = QStringLiteral("https://x.org/clip.mp4");
    check("a downloaded video", clipboardImageUrl(localVideo).isEmpty());
    Post localImage;
    localImage.localFilePath = QStringLiteral("/home/u/pic.png");
    equal("a downloaded image", clipboardImageUrl(localImage), QStringLiteral("file:///home/u/pic.png"));
}

void testCookieStaysOnItsSite()
{
    begin("A site's cookie does not follow a redirect to another site");

    // The server is known as 127.0.0.1 and as localhost: two hosts to Qt.
    RouteServer *self = nullptr;
    RouteServer server([&self](const QString &path, const QUrlQuery &) {
        RouteServer::Answer answer;
        if (path == QStringLiteral("/away")) {
            answer.status = 302;
            answer.location = QStringLiteral("http://localhost:%1/landed").arg(self->serverPort()).toLatin1();
        } else if (path == QStringLiteral("/within")) {
            answer.status = 302;
            answer.location = QStringLiteral("http://127.0.0.1:%1/landed").arg(self->serverPort()).toLatin1();
        } else if (path == QStringLiteral("/landed")) {
            answer = data("arrived", "text/plain");
        }
        return answer;
    });
    self = &server;

    QNetworkAccessManager network;
    struct Outcome {
        bool ok = false;
        QByteArray body;
        QString refused;
    };
    const auto fetch = [&network, &server](const QString &path, const QByteArray &ownCookie = {}) {
        QNetworkRequest request{QUrl(server.base() + path)};
        request.setTransferTimeout(5000);
        if (!ownCookie.isEmpty())
            request.setRawHeader("Cookie", ownCookie);
        QNetworkReply *reply = SiteAccess::get(&network, request);
        waitFor(8000, [reply]() { return reply->isFinished(); });
        Outcome outcome;
        outcome.ok = reply->isFinished() && reply->error() == QNetworkReply::NoError;
        outcome.body = reply->readAll();
        outcome.refused = SiteAccess::refusedRedirect(reply);
        reply->deleteLater();
        return outcome;
    };

    QHash<QString, SiteAccess::Credentials> byHost;
    byHost.insert(QStringLiteral("127.0.0.1"), {QStringLiteral("session=secret"), QString()});
    SiteAccess::setAll(byHost);

    const Outcome away = fetch(QStringLiteral("/away"));
    equal("the site itself gets the cookie", server.cookiesSentTo(QStringLiteral("/away")).join(QLatin1Char('|')),
          QStringLiteral("session=secret"));
    check("a redirect to another host is not followed", !away.ok && server.cookiesSentTo(QStringLiteral("/landed")).isEmpty(),
          server.cookiesSentTo(QStringLiteral("/landed")).join(QLatin1Char('|')));
    check("and is reported as such", QUrl(away.refused).host() == QStringLiteral("localhost"), away.refused);

    const Outcome within = fetch(QStringLiteral("/within"));
    check("a redirect within the site is followed", within.ok && within.body == "arrived");
    equal("with the cookie", server.cookiesSentTo(QStringLiteral("/landed")).join(QLatin1Char('|')),
          QStringLiteral("session=secret"));
    check("and nothing is reported", within.refused.isEmpty());

    // A cookie the request brought along itself (a source's own, a plugin's).
    SiteAccess::setAll({});
    server.rawRequests.clear();
    const Outcome own = fetch(QStringLiteral("/away"), "own=1");
    check("a cookie set by the source stays as well",
          !own.ok && server.cookiesSentTo(QStringLiteral("/landed")).isEmpty() && !own.refused.isEmpty());

    // Without a cookie there is nothing to protect: redirects work as ever.
    server.rawRequests.clear();
    const Outcome plain = fetch(QStringLiteral("/away"));
    check("a request without a cookie follows any redirect", plain.ok && plain.body == "arrived");
    equal("and still carries none", server.cookiesSentTo(QStringLiteral("/landed")).join(QLatin1Char('|')),
          QStringLiteral("-"));
}

void testDownloadQueue()
{
    begin("A large batch of downloads waits its turn instead of timing out");

    QTemporaryDir dir;
    // Every file takes 600 ms to answer; a download is given up after 2 s
    // without data. Thirty of them at six per host take five rounds, 3 s:
    // sent all at once, the last two rounds would time out while they wait
    // inside Qt, without ever reaching the server.
    RouteServer server([](const QString &, const QUrlQuery &) {
        RouteServer::Answer answer = data(TestHttpServer::okBody(), "image/png");
        answer.delayMs = 600;
        return answer;
    });
    if (!dir.isValid() || !server.isListening()) {
        check("temporary directory and test server", false);
        return;
    }

    DownloadManager manager(nullptr);
    manager.setDownloadDir(dir.path());
    manager.setTransferTimeout(2000);

    int saved = 0;
    QStringList errors;
    QObject::connect(&manager, &DownloadManager::downloadFinished,
        [&saved, &errors](const QString &, bool ok, bool, const QString &error) {
            if (ok)
                ++saved;
            else
                errors.append(error);
        });

    constexpr int total = 30;
    for (int i = 0; i < total; ++i) {
        Post post;
        post.id = QString::number(i);
        post.sourceName = QStringLiteral("Batch");
        post.fileUrl = server.base() + QStringLiteral("/f/%1.png").arg(i);
        manager.download(post);
    }
    check("only a few start at once", manager.runningCount() == DownloadManager::kMaxParallel,
          QString::number(manager.runningCount()));
    check("the rest wait", manager.queuedCount() == total - DownloadManager::kMaxParallel,
          QString::number(manager.queuedCount()));

    // The same post again while it waits is not queued twice.
    Post again;
    again.id = QString::number(total - 1);
    again.sourceName = QStringLiteral("Batch");
    again.fileUrl = server.base() + QStringLiteral("/f/%1.png").arg(total - 1);
    manager.download(again);
    check("a waiting download is not queued twice", manager.queuedCount() == total - DownloadManager::kMaxParallel);

    waitFor(20000, [&]() { return saved + errors.size() >= total; });
    check("every file was saved", saved == total,
          QStringLiteral("%1 saved, %2 failed: %3").arg(saved).arg(errors.size()).arg(errors.value(0)));
    check("the server never had more than the limit open", server.mostOpenRequests <= DownloadManager::kMaxParallel,
          QString::number(server.mostOpenRequests));
    check("every file is on disk",
          QDir(dir.path()).entryList({QStringLiteral("Batch_*.png")}, QDir::Files).size() == total);
    check("nothing is left running or waiting", manager.runningCount() == 0 && manager.queuedCount() == 0);

    // The download folder disappears while the program runs (deleted in a
    // file manager): it used to be refused as "outside the allowed folders".
    const QString vanishing = dir.filePath(QStringLiteral("vanishing"));
    QDir().mkpath(vanishing);
    manager.setDownloadDir(vanishing);
    QDir(vanishing).removeRecursively();
    saved = 0;
    errors.clear();
    Post post;
    post.id = QStringLiteral("late");
    post.sourceName = QStringLiteral("Batch");
    post.fileUrl = server.base() + QStringLiteral("/f/late.png");
    manager.download(post);
    waitFor(10000, [&]() { return saved + errors.size() >= 1; });
    check("a vanished download folder is created again", saved == 1 && QFile::exists(vanishing + QStringLiteral("/Batch_late.png")),
          errors.value(0));
}

QByteArray pngOfSize(int width, int height, const QColor &color = Qt::darkCyan)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString sizeText(const QSize &size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

#ifdef Q_OS_LINUX
// The most memory this process has held so far, in megabytes.
int peakMemoryMb()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> lines = status.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (line.startsWith("VmHWM:"))
            return line.mid(6).simplified().split(' ').value(0).toInt() / 1024;
    }
    return -1;
}
#endif

void testThumbnailDecoding()
{
    begin("Thumbnails are decoded at the size they are shown at");

    const auto decoded = [](const QByteArray &bytes, const QSize &target) {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        return ThumbnailLoader::decodeScaled(&buffer, target);
    };
    equal("a wide image fits the width", sizeText(decoded(pngOfSize(400, 200), QSize(100, 100)).size()),
          QStringLiteral("100x50"));
    equal("a tall image fits the height", sizeText(decoded(pngOfSize(200, 400), QSize(100, 100)).size()),
          QStringLiteral("50x100"));
    equal("a small image fills the cell, as before", sizeText(decoded(pngOfSize(20, 10), QSize(100, 100)).size()),
          QStringLiteral("100x50"));
    check("what is no image gives nothing", decoded("neither png nor jpeg", QSize(100, 100)).isNull());
    const QImage red = decoded(pngOfSize(300, 300, Qt::red), QSize(64, 64));
    check("the picture survives the scaling", !red.isNull() && (red.pixel(32, 32) & 0x00ffffff) == 0x00ff0000);

    QTemporaryDir dir;
    const QString picture = dir.filePath(QStringLiteral("picture.png"));
    check("test image written", dir.isValid() && writeFile(picture, pngOfSize(400, 200)));

    // What the library tab does: a downloaded file is its own thumbnail.
    ThumbnailLoader loader(nullptr, /*useDiskCache=*/false);
    loader.setTargetSize(QSize(100, 100));
    QHash<int, QSize> delivered;
    QObject::connect(&loader, &ThumbnailLoader::thumbnailReady,
                     [&delivered](int row, const QPixmap &pixmap) { delivered.insert(row, pixmap.size()); });
    loader.request(0, QUrl::fromLocalFile(picture));
    waitFor(5000, [&delivered]() { return delivered.contains(0); });
    equal("a local file is read from disk at thumbnail size", sizeText(delivered.value(0)), QStringLiteral("100x50"));

    loader.request(1, QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png"))));
    spinFor(300);
    check("a missing file gives nothing", !delivered.contains(1));

#ifdef Q_OS_LINUX
    // A downloaded file that is no image used to be read whole into memory,
    // only for the decoder to reject it: 300 MB per cell for a video. The
    // file here is 192 MB of nothing (sparse, so it takes no disk space).
    // Its name is not a video's on purpose: a video would be handed to the
    // system's media player, whose start-up alone allocates more than that
    // on some machines, and this test is about the reading.
    const QString big = dir.filePath(QStringLiteral("archive.zip"));
    {
        QFile file(big);
        check("sparse test file created", file.open(QIODevice::WriteOnly) && file.resize(192LL * 1024 * 1024));
    }
    const int before = peakMemoryMb();
    for (int row = 2; row < 6; ++row)
        loader.request(row, QUrl::fromLocalFile(big));
    spinFor(1000);
    const int after = peakMemoryMb();
    check("a local file that is no image is not read into memory", before > 0 && after - before < 96,
          QStringLiteral("peak grew by %1 MB").arg(after - before));
    check("and gets no thumbnail", !delivered.contains(2) && !delivered.contains(5));
#endif
}

void testThumbnailReload()
{
    begin("Thumbnails are fetched again when their size changes");

    QTemporaryDir dir;
    QVector<Post> posts;
    for (int row = 0; row < 3; ++row) {
        const QString path = dir.filePath(QStringLiteral("p%1.png").arg(row));
        writeFile(path, pngOfSize(600, 600));
        Post post;
        post.id = QString::number(row);
        post.localFilePath = path;
        post.previewUrl = QUrl::fromLocalFile(path).toString();
        posts.append(post);
    }

    GalleryModel model;
    model.appendPosts(posts);
    QListView view;
    GalleryView::configure(&view);
    view.setModel(&model);
    const QSize smallCell(108, 108);
    const QSize largeCell(208, 208);
    GalleryView::setCellSize(&view, smallCell);
    view.resize(700, 500);
    view.show();

    ThumbnailLoader loader(nullptr, /*useDiskCache=*/false);
    loader.setTargetSize(GalleryView::thumbnailSizeForCell(smallCell));
    QObject::connect(&loader, &ThumbnailLoader::thumbnailReady,
                     [&model](int row, const QPixmap &pixmap) { model.setThumbnail(row, pixmap); });
    const auto widths = [&model]() {
        QStringList list;
        for (int row = 0; row < model.rowCount(); ++row)
            list << QString::number(model.thumbnailAt(row).width());
        return list.join(QLatin1Char(','));
    };

    GalleryView::requestVisibleRows(&view, &model, &loader);
    waitFor(5000, [&]() { return widths() == QStringLiteral("100,100,100"); });
    equal("thumbnails arrive at the size of the cell", widths(), QStringLiteral("100,100,100"));

    // The slider moved. Asking for the visible rows alone changes nothing:
    // they count as fetched, which is why they used to stay small and blurred.
    loader.setTargetSize(GalleryView::thumbnailSizeForCell(largeCell));
    GalleryView::setCellSize(&view, largeCell);
    GalleryView::requestVisibleRows(&view, &model, &loader);
    spinFor(400);
    equal("rows already fetched are not fetched twice by scrolling", widths(), QStringLiteral("100,100,100"));
    check("the old thumbnails are painted stretched meanwhile", !view.grab().isNull());

    GalleryView::reloadThumbnails(&view, &model, &loader);
    equal("the old thumbnails stay until the new ones arrive", widths(), QStringLiteral("100,100,100"));
    waitFor(5000, [&]() { return widths() == QStringLiteral("200,200,200"); });
    equal("a reload brings them at the new size", widths(), QStringLiteral("200,200,200"));
    check("and they are painted as they are", !view.grab().isNull());
}

void testLibrarySeveralTags()
{
    begin("Library search with several tags");
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/library.sqlite3"));
    LibraryDatabase db;
    if (!db.isOpen()) {
        check("database opens", false);
        return;
    }

    // Thirty posts, all "common"; three of them "rare"; two "Mixed_Case".
    const QString root = QDir::tempPath() + QStringLiteral("/blairqt-test-tags");
    for (int i = 1; i <= 30; ++i) {
        Post p;
        p.id = QString::number(i);
        p.sourceName = QStringLiteral("Src");
        p.tags = QStringList{QStringLiteral("common")};
        if (i == 5 || i == 15 || i == 25)
            p.tags << QStringLiteral("rare");
        if (i == 10 || i == 15)
            p.tags << QStringLiteral("Mixed_Case");
        const QString folder = (i % 2 == 0) ? QStringLiteral("/even/") : QStringLiteral("/");
        db.addOrUpdatePost(p, root + folder + QStringLiteral("%1.jpg").arg(i));
    }
    const auto ids = [](const QVector<Post> &posts) {
        QStringList list;
        for (const Post &post : posts)
            list << post.id;
        return list.join(QLatin1Char(','));
    };

    equal("both tags, newest first", ids(db.search(QStringLiteral("common rare"), -1, 0)), QStringLiteral("25,15,5"));
    equal("the order of the terms does not matter", ids(db.search(QStringLiteral("rare common"), -1, 0)),
          QStringLiteral("25,15,5"));
    equal("case does not matter", ids(db.search(QStringLiteral("RARE Common mixed_case"), -1, 0)),
          QStringLiteral("15"));
    equal("a term twice is one term", ids(db.search(QStringLiteral("rare rare Rare"), -1, 0)), QStringLiteral("25,15,5"));
    check("a tag nobody has finds nothing, wherever it stands",
          db.search(QStringLiteral("common missing"), -1, 0).isEmpty()
              && db.search(QStringLiteral("missing common"), -1, 0).isEmpty());
    check("a prefix of a tag is not the tag", db.search(QStringLiteral("comm rare"), -1, 0).isEmpty());

    // Paging by cursor, with tags.
    const QVector<Post> first = db.search(QStringLiteral("common rare"), 2, 0);
    equal("first page", ids(first), QStringLiteral("25,15"));
    equal("second page continues below the cursor",
          ids(db.search(QStringLiteral("common rare"), 2, first.last().libraryId)), QStringLiteral("5"));

    QStringList seen;
    qint64 cursor = 0;
    for (int page = 0; page < 5; ++page) {
        const QVector<Post> posts = db.search(QStringLiteral("common"), 10, cursor);
        if (posts.isEmpty())
            break;
        for (const Post &post : posts)
            seen << post.id;
        cursor = posts.last().libraryId;
    }
    check("three pages of ten cover all thirty once", seen.size() == 30 && QSet<QString>(seen.begin(), seen.end()).size() == 30,
          QString::number(seen.size()));
    equal("in descending order", seen.mid(0, 3).join(QLatin1Char(',')), QStringLiteral("30,29,28"));

    // Tags together with the folder filter.
    equal("tags inside one folder", ids(db.search(QStringLiteral("common rare"), -1, 0, root, false)),
          QStringLiteral("25,15,5"));
    equal("tags inside a subfolder", ids(db.search(QStringLiteral("common Mixed_Case"), -1, 0, root + QStringLiteral("/even"), false)),
          QStringLiteral("10"));
    check("tags in a folder that has none of them",
          db.search(QStringLiteral("rare"), -1, 0, root + QStringLiteral("/even"), false).isEmpty());
    equal("tags with subfolders included", ids(db.search(QStringLiteral("Mixed_Case"), -1, 0, root, true)),
          QStringLiteral("15,10"));

    // A post whose tags changed is found by the new ones only.
    Post changed;
    changed.id = QStringLiteral("25");
    changed.sourceName = QStringLiteral("Src");
    changed.tags = QStringList{QStringLiteral("common"), QStringLiteral("other")};
    db.addOrUpdatePost(changed, root + QStringLiteral("/25.jpg"));
    equal("after retagging", ids(db.search(QStringLiteral("rare common"), -1, 0)), QStringLiteral("15,5"));
}

// Presses a button of the message box that is about to open.
void answerNextMessageBox(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button]() {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(button)->click();
    });
}

// A booru for the search tab, speaking the gelbooru dapi: five posts per
// page for ever, except where told to fail.
class FakeBooru {
public:
    FakeBooru()
        : server([this](const QString &path, const QUrlQuery &query) { return answer(path, query); })
    {
    }

    RouteServer server;
    // The pages asked for, in order (dapi counts them from 0).
    QList<int> pagesAsked;
    // Pages answered with HTTP 500: once, or every time.
    QSet<int> failOnce;
    QSet<int> failAlways;

private:
    RouteServer::Answer answer(const QString &path, const QUrlQuery &query)
    {
        if (path != QStringLiteral("/index.php"))
            return html("no such file", 404);
        // A request with an API key is turned away: the address then carries
        // a secret that must not show up in an error message.
        if (query.hasQueryItem(QStringLiteral("api_key")))
            return html("forbidden", 403);

        const int page = query.queryItemValue(QStringLiteral("pid")).toInt();
        pagesAsked.append(page);
        if (failAlways.contains(page) || failOnce.remove(page))
            return html("{\"message\":\"database is busy\"}", 500);

        QByteArray json = "[";
        for (int i = 0; i < 5; ++i) {
            const QByteArray id = QByteArray::number(page * 100 + i);
            json += (i ? "," : "") + QByteArray("{\"id\":") + id + ",\"tags\":\"tag_a tag_b\",\"file_url\":\""
                + server.base().toLatin1() + "/img/" + id + ".png\",\"preview_url\":\""
                + server.base().toLatin1() + "/img/" + id + ".png\"}";
        }
        return data(json + "]");
    }
};

void testSearchTab()
{
    begin("Search tab: errors, pages and sources");

    FakeBooru booru;
    DroppingServer down;
    if (!booru.server.isListening() || !down.isListening()) {
        check("test servers", false);
        return;
    }

    // Only sources that live on this machine: nothing here may reach a real site.
    SourceManager manager;
    QStringList builtIn;
    for (const auto &source : manager.sources())
        builtIn << source->name();
    for (const QString &name : std::as_const(builtIn))
        manager.removeSource(name);
    manager.addCustomSource(QStringLiteral("Local"), QStringLiteral("gelbooru"), booru.server.base());
    manager.addCustomSource(QStringLiteral("Dead"), QStringLiteral("gelbooru"), down.base());
    manager.addCustomSource(QStringLiteral("Keyed"), QStringLiteral("gelbooru"), booru.server.base(),
                            QStringLiteral("SECRETKEY"), QStringLiteral("42"));

    QSettings settings;
    settings.setValue(QStringLiteral("welcomeCompleted"), true);
    settings.setValue(QStringLiteral("startupSources"), QStringList{QStringLiteral("Local")});
    settings.setValue(QStringLiteral("resultsPerPage"), 5);
    Blacklist::setGlobalTags({});

    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    {
        LibraryDatabase library;
        DownloadManager downloads(&library);
        SearchTab tab(manager, &library, &downloads);

        QStringList statuses;
        QObject::connect(&tab, &SearchTab::statusMessage, [&statuses](const QString &text) { statuses << text; });
        QStringList logged;
        const QMetaObject::Connection logging = QObject::connect(&NetworkLog::instance(), &NetworkLog::entryAdded,
            [&logged](const QString &line) { logged << line; });

        auto *combo = tab.findChild<QComboBox *>(QStringLiteral("sourceCombo"));
        auto *searchButton = tab.findChild<QPushButton *>(QStringLiteral("searchButton"));
        auto *loadMore = tab.findChild<QPushButton *>(QStringLiteral("loadMoreButton"));
        auto *nextPage = tab.findChild<QPushButton *>(QStringLiteral("nextPageButton"));
        auto *deleteSource = tab.findChild<QPushButton *>(QStringLiteral("deleteSourceButton"));
        auto *multiSource = tab.findChild<QPushButton *>(QStringLiteral("multiSourceButton"));
        auto *autoscroll = tab.findChild<QCheckBox *>(QStringLiteral("autoscrollCheck"));
        auto *gallery = tab.findChild<QListView *>(QStringLiteral("galleryView"));
        if (!combo || !searchButton || !loadMore || !nextPage || !deleteSource || !multiSource || !autoscroll || !gallery) {
            check("the tab's controls are found", false);
            return;
        }
        // The search button is disabled exactly while a round is running.
        const auto idle = [searchButton]() { return searchButton->isEnabled(); };
        const auto rows = [gallery]() { return gallery->model()->rowCount(); };
        const auto pages = [&booru]() {
            QStringList list;
            for (const int page : std::as_const(booru.pagesAsked))
                list << QString::number(page);
            return list.join(QLatin1Char(','));
        };

        tab.resize(900, 700);
        tab.show();
        tab.start();
        waitFor(8000, idle);
        check("the first page is shown", rows() == 5, QString::number(rows()));

        // --- A page that fails is asked for again, not skipped.
        booru.pagesAsked.clear();
        booru.failOnce = {1};
        loadMore->click();
        waitFor(8000, idle);
        check("a failed page adds nothing", rows() == 5, QString::number(rows()));
        check("the error is what the status bar is left showing",
              statuses.last().startsWith(QStringLiteral("Błąd (Local)")) && statuses.last().contains(QStringLiteral("HTTP 500")),
              statuses.last());
        check("the site's own explanation is in the log",
              logged.join(QLatin1Char('\n')).contains(QStringLiteral("database is busy")));
        loadMore->click();
        waitFor(8000, idle);
        check("\"load more\" asks for the failed page again", pages() == QStringLiteral("1,1") && rows() == 10, pages());
        loadMore->click();
        waitFor(8000, idle);
        check("and then moves on", pages() == QStringLiteral("1,1,2") && rows() == 15, pages());

        booru.pagesAsked.clear();
        booru.failOnce = {3};
        nextPage->click();
        waitFor(8000, idle);
        nextPage->click();
        waitFor(8000, idle);
        check("\"next page\" asks for the failed page again too", pages() == QStringLiteral("3,3") && rows() == 5, pages());

        // --- Autoscroll must not ask for a failing page on every tick.
        booru.pagesAsked.clear();
        booru.failAlways = {4};
        autoscroll->setChecked(true);
        waitFor(8000, [autoscroll]() { return !autoscroll->isChecked(); });
        spinFor(500);
        check("autoscroll stops when the next page fails", !autoscroll->isChecked());
        check("after asking once", pages() == QStringLiteral("4"), pages());
        check("and says so", statuses.last().contains(QStringLiteral("Autoscroll")), statuses.last());
        booru.failAlways.clear();

        // --- A site that cannot be reached.
        combo->setCurrentIndex(combo->findText(QStringLiteral("Dead")));
        waitFor(8000, idle);
        check("a dead site leaves its error in the status bar", statuses.last().startsWith(QStringLiteral("Błąd (Dead)")),
              statuses.last());
        check("the view is empty", rows() == 0);

        // --- An error names no address: the address carries the API key.
        logged.clear();
        combo->setCurrentIndex(combo->findText(QStringLiteral("Keyed")));
        waitFor(8000, idle);
        check("the refusal is reported", statuses.last().contains(QStringLiteral("HTTP 403")), statuses.last());
        check("without the API key in the status bar", !statuses.join(QLatin1Char('\n')).contains(QStringLiteral("SECRETKEY")));
        check("and without it in the log", !logged.join(QLatin1Char('\n')).contains(QStringLiteral("SECRETKEY")),
              logged.join(QStringLiteral(" / ")));

        // --- Several sources at once, one of them dead.
        QTimer::singleShot(0, []() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *list = dialog ? dialog->findChild<QListWidget *>() : nullptr;
            if (!list)
                return;
            for (int i = 0; i < list->count(); ++i) {
                const QString name = list->item(i)->text();
                list->item(i)->setCheckState(name == QStringLiteral("Local") || name == QStringLiteral("Dead")
                                                 ? Qt::Checked : Qt::Unchecked);
            }
            dialog->accept();
        });
        multiSource->click();
        waitFor(8000, idle);
        check("the live source's posts are shown", rows() == 5, QString::number(rows()));
        check("and the dead one is named", statuses.last().contains(QStringLiteral("bez odpowiedzi: Dead")), statuses.last());

        QObject::disconnect(logging);
    }

    // --- With no startup source the combo shows its first source unselected.
    settings.setValue(QStringLiteral("startupSources"), QStringList());
    manager.removeSource(QStringLiteral("Dead"));
    manager.removeSource(QStringLiteral("Keyed"));
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    {
        LibraryDatabase library;
        DownloadManager downloads(&library);
        SearchTab tab(manager, &library, &downloads);
        QStringList statuses;
        QObject::connect(&tab, &SearchTab::statusMessage, [&statuses](const QString &text) { statuses << text; });
        auto *combo = tab.findChild<QComboBox *>(QStringLiteral("sourceCombo"));
        auto *searchButton = tab.findChild<QPushButton *>(QStringLiteral("searchButton"));
        auto *deleteSource = tab.findChild<QPushButton *>(QStringLiteral("deleteSourceButton"));
        auto *gallery = tab.findChild<QListView *>(QStringLiteral("galleryView"));
        const auto idle = [searchButton]() { return searchButton->isEnabled(); };

        tab.resize(900, 700);
        tab.show();
        tab.start();
        spinFor(200);
        check("nothing is searched without a startup source", gallery->model()->rowCount() == 0 && !deleteSource->isEnabled());
        equal("the combo shows the only source", combo->currentText(), QStringLiteral("Local"));

        // Picking the entry already shown changes no index; it must still work.
        emit combo->activated(combo->currentIndex());
        waitFor(8000, idle);
        check("picking the source already shown searches it", gallery->model()->rowCount() == 5,
              QString::number(gallery->model()->rowCount()));
        check("and makes it the one \"delete\" acts on", deleteSource->isEnabled());

        // --- Deleting the last source.
        answerNextMessageBox(QMessageBox::Yes);
        deleteSource->click();
        spinFor(200);
        check("the last source is gone", manager.sources().isEmpty());
        check("the view is emptied", gallery->model()->rowCount() == 0);
        check("\"delete\" has nothing left to act on", !deleteSource->isEnabled());
        check("only \"add a booru\" is left in the combo", combo->count() == 1);
        check("the search button is not left disabled", searchButton->isEnabled());

        // The one entry left is already the current one: it must open the
        // dialog anyway, or there is no way to add a source from here.
        QString opened;
        QTimer::singleShot(0, [&opened]() {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                opened = QString::fromLatin1(dialog->metaObject()->className());
                dialog->reject();
            }
        });
        emit combo->activated(0);
        equal("picking it opens the \"add a booru\" dialog", opened, QStringLiteral("AddSourceDialog"));

        // --- Restoring the defaults brings the list back and searches nothing.
        tab.restoreDefaultSources();
        spinFor(200);
        check("the default sources are back", manager.sources().size() == builtIn.size(),
              QString::number(manager.sources().size()));
        check("one of them is selected again", deleteSource->isEnabled() && combo->count() == builtIn.size() + 1);
        check("without searching it unasked", gallery->model()->rowCount() == 0 && searchButton->isEnabled());
    }

    settings.remove(QStringLiteral("startupSources"));
    settings.remove(QStringLiteral("welcomeCompleted"));
    settings.remove(QStringLiteral("resultsPerPage"));
}

void testLibraryTab()
{
    begin("Library tab: new downloads, deleting");

    QTemporaryDir inside;  // the download folder
    QTemporaryDir outside; // a folder the program was never given
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/library.sqlite3"));
    LibraryDatabase library;
    if (!inside.isValid() || !outside.isValid() || !library.isOpen()) {
        check("temporary directories and database", false);
        return;
    }
    Blacklist::setAppliesToLibrary(false);

    DownloadManager downloads(&library);
    downloads.setDownloadDir(inside.path());

    int nextId = 0;
    const auto addFile = [&library, &nextId](const QString &path) {
        writeFile(path, pngOfSize(64, 64));
        Post post;
        post.id = QString::number(++nextId);
        post.sourceName = QStringLiteral("Src");
        post.tags = QStringList{QStringLiteral("tag_a")};
        library.addOrUpdatePost(post, path);
    };
    const QString keptOutside = outside.filePath(QStringLiteral("elsewhere.png"));
    const QString firstInside = inside.filePath(QStringLiteral("first.png"));
    addFile(keptOutside);
    addFile(firstInside);

    LibraryTab tab(&library, &downloads);
    auto *view = tab.findChild<QListView *>(QStringLiteral("libraryView"));
    auto *status = tab.findChild<QLabel *>(QStringLiteral("libraryStatusLabel"));
    if (!view || !status) {
        check("the tab's widgets are found", false);
        return;
    }
    const auto rows = [view]() { return view->model()->rowCount(); };

    tab.resize(900, 700);
    tab.show();
    tab.refreshLibraryFolderTreeRoot();
    tab.start();
    check("both files are listed", rows() == 2, QString::number(rows()));

    // --- A finished download shows up without searching again by hand.
    addFile(inside.filePath(QStringLiteral("second.png")));
    tab.noteLibraryChanged();
    check("not on the spot: a batch finishes file by file", rows() == 2);
    waitFor(5000, [&rows]() { return rows() == 3; });
    check("a new download appears in the list", rows() == 3, QString::number(rows()));

    // --- But not while the user has files selected: the list would be
    // read again and the selection lost.
    view->selectionModel()->select(view->model()->index(0, 0), QItemSelectionModel::ClearAndSelect);
    addFile(inside.filePath(QStringLiteral("third.png")));
    tab.noteLibraryChanged();
    spinFor(1200);
    check("a selection is left alone", rows() == 3 && view->selectionModel()->hasSelection());
    tab.hide();
    tab.show();
    check("the list catches up when the tab is shown again", rows() == 4, QString::number(rows()));

    // --- Deleting: a file outside the allowed folders is only taken off the
    // list, and the status must not call that "deleted".
    check("the outside folder is not an allowed one", !FolderAccessGuard::isAllowed(keptOutside));
    // Newest first: third, second, first, elsewhere.
    view->selectionModel()->clearSelection();
    view->selectionModel()->select(view->model()->index(2, 0), QItemSelectionModel::Select);
    view->selectionModel()->select(view->model()->index(3, 0), QItemSelectionModel::Select);
    answerNextMessageBox(QMessageBox::Yes);
    QMetaObject::invokeMethod(&tab, "deleteSelectedLibraryItems");
    check("both rows leave the list", rows() == 2 && library.count() == 2,
          QStringLiteral("%1 rows, %2 in the database").arg(rows()).arg(library.count()));
    check("the file inside the download folder is deleted", !QFile::exists(firstInside));
    check("the file outside stays on disk", QFile::exists(keptOutside));
    check("the status says one file was deleted", status->text().startsWith(QStringLiteral("Usunięto 1 ")), status->text());
    check("and that the other is still on disk", status->text().contains(QStringLiteral("tylko z biblioteki")), status->text());
}


// Fills in and accepts the text question that is about to open; polls,
// since it may open only after a queued call.
void answerNextInputDialog(const QString &text, bool *answered = nullptr)
{
    auto *poll = new QTimer();
    poll->setInterval(20);
    auto tries = std::make_shared<int>(0);
    QObject::connect(poll, &QTimer::timeout, [poll, text, answered, tries]() {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(text);
            dialog->accept();
            if (answered)
                *answered = true;
            poll->deleteLater();
        } else if (++*tries > 250) {
            poll->deleteLater();
        }
    });
    poll->start();
}

QStringList sidecarOf(const QString &imagePath)
{
    QFile file(DownloadManager::sidecarPathFor(imagePath));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

void testLibraryImport()
{
    begin("Importing pictures from anywhere into the library");

    QTemporaryDir library; // the download folder
    QTemporaryDir outside; // the user's other pictures
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/library.sqlite3"));
    LibraryDatabase db;
    if (!library.isValid() || !outside.isValid() || !db.isOpen()) {
        check("temporary directories and database", false);
        return;
    }
    DownloadManager downloads(&db);
    downloads.setDownloadDir(library.path()); // what makes it an allowed folder

    check("pictures and videos count as media", LibraryImport::isMedia(QStringLiteral("/x/a.PNG"))
              && LibraryImport::isMedia(QStringLiteral("a.jpg")) && LibraryImport::isMedia(QStringLiteral("a.webm")));
    check("other files do not", !LibraryImport::isMedia(QStringLiteral("notes.txt"))
              && !LibraryImport::isMedia(QStringLiteral("archive.zip")) && !LibraryImport::isMedia(QStringLiteral("noext")));

    // a.png with its own tags sidecar; a copy of it under another name; a
    // folder with one more picture; a video; a text file.
    const QString a = outside.filePath(QStringLiteral("a.png"));
    writeFile(a, pngOfSize(40, 30, Qt::red));
    DownloadManager::writeSidecar(DownloadManager::sidecarPathFor(a), {QStringLiteral("red_hair")});
    const QString copyOfA = outside.filePath(QStringLiteral("same_as_a.png"));
    QFile::copy(a, copyOfA);
    QDir(outside.path()).mkpath(QStringLiteral("sub/deeper"));
    const QString c = outside.filePath(QStringLiteral("sub/deeper/c.png"));
    writeFile(c, pngOfSize(20, 20, Qt::blue));
    const QString clip = outside.filePath(QStringLiteral("clip.mp4"));
    writeFile(clip, QByteArray("not really a video, but named like one"));
    const QString notes = outside.filePath(QStringLiteral("notes.txt"));
    writeFile(notes, QByteArray("hello"));

    const QString target = library.filePath(QStringLiteral("imported"));
    const LibraryImport::Result result = LibraryImport::importPaths(&db,
        {a, outside.filePath(QStringLiteral("sub")), clip, notes, copyOfA}, target, library.path(),
        {QStringLiteral("mine")});
    check("pictures, a folder's pictures and a video are imported", result.imported == 3,
          QString::number(result.imported) + QStringLiteral(" / ") + result.errors.join(QStringLiteral("; ")));
    check("the same file under another name is recognised", result.alreadyInLibrary == 1);
    check("a text file is left out", result.notMedia == 1);
    check("nothing failed", result.failed == 0 && result.errors.isEmpty(), result.errors.join(QStringLiteral("; ")));
    check("the originals stay where they were", QFile::exists(a) && QFile::exists(c) && QFile::exists(clip));
    check("the copies are in the chosen folder", QFile::exists(target + QStringLiteral("/a.png"))
              && QFile::exists(target + QStringLiteral("/c.png")) && QFile::exists(target + QStringLiteral("/clip.mp4")));
    equal("the file's own tags and the given ones", sidecarOf(target + QStringLiteral("/a.png")).join(QLatin1Char(' ')),
          QStringLiteral("red_hair mine"));
    check("the library finds them by tag", db.search(QStringLiteral("mine"), -1, 0).size() == 3
              && db.search(QStringLiteral("red_hair mine"), -1, 0).size() == 1);
    const QVector<Post> red = db.search(QStringLiteral("red_hair"), -1, 0);
    check("with the picture's size and a source of its own", red.size() == 1 && red.first().width == 40
              && red.first().height == 30 && red.first().sourceName == LibraryImport::kSourceName);

    // Again: nothing new.
    const LibraryImport::Result again = LibraryImport::importPaths(&db, {a}, target, library.path());
    check("importing the same file again adds nothing", again.imported == 0 && again.alreadyInLibrary == 1
              && !QFile::exists(target + QStringLiteral("/a (2).png")));

    // Another picture with a name already taken.
    QDir(outside.path()).mkpath(QStringLiteral("other"));
    const QString otherA = outside.filePath(QStringLiteral("other/a.png"));
    writeFile(otherA, pngOfSize(10, 10, Qt::green));
    const LibraryImport::Result named = LibraryImport::importPaths(&db, {otherA}, target, library.path());
    check("a taken name gets a number", named.imported == 1 && QFile::exists(target + QStringLiteral("/a (2).png"))
              && named.importedPaths.value(0) == target + QStringLiteral("/a (2).png"));

    // A download already in the library keeps its site and tags.
    const QString downloaded = library.filePath(QStringLiteral("Danbooru_1.png"));
    writeFile(downloaded, pngOfSize(12, 12, Qt::yellow));
    Post post;
    post.id = QStringLiteral("1");
    post.sourceName = QStringLiteral("Danbooru");
    post.tags = QStringList{QStringLiteral("from_the_site")};
    db.addOrUpdatePost(post, downloaded);
    // Stored the way a download folder set with a trailing slash writes it.
    const QString doubleSlashed = library.path() + QStringLiteral("//Danbooru_2.png");
    writeFile(library.filePath(QStringLiteral("Danbooru_2.png")), pngOfSize(13, 13, Qt::cyan));
    post.id = QStringLiteral("2");
    db.addOrUpdatePost(post, doubleSlashed);
    const LibraryImport::Result inPlace = LibraryImport::importPaths(&db,
        {downloaded, library.filePath(QStringLiteral("Danbooru_2.png"))}, target, library.path(), {QStringLiteral("mine")});
    check("a file the library has is not imported twice", inPlace.imported == 0 && inPlace.alreadyInLibrary == 2);
    equal("and keeps its site", db.pathForPost(QStringLiteral("Danbooru"), QStringLiteral("1")), downloaded);
    check("and its tags", db.search(QStringLiteral("from_the_site"), -1, 0).size() == 2
              && db.search(QStringLiteral("from_the_site mine"), -1, 0).isEmpty());

    // A file put into the library's folders by hand is indexed where it is.
    const QString byHand = library.filePath(QStringLiteral("by_hand.png"));
    writeFile(byHand, pngOfSize(14, 14, Qt::magenta));
    const LibraryImport::Result here = LibraryImport::importPaths(&db, {byHand}, target, library.path());
    check("a file already in the library's folders is indexed in place", here.imported == 1
              && here.importedPaths.value(0) == byHand && !QFile::exists(target + QStringLiteral("/by_hand.png")));

    // Nothing is written outside the allowed folders.
    QTemporaryDir forbidden;
    const LibraryImport::Result refused = LibraryImport::importPaths(&db, {otherA}, forbidden.path(), library.path());
    check("a folder outside the allowed ones is refused", refused.imported == 0 && !refused.errors.isEmpty()
              && QDir(forbidden.path()).entryList(QDir::Files).isEmpty());
}

void testLibraryTabImport()
{
    begin("Library tab: importing with the button and by dropping");

    QTemporaryDir library;
    QTemporaryDir outside;
    QSqlDatabase::removeDatabase(QStringLiteral("blairqt_library"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/library.sqlite3"));
    LibraryDatabase db;
    if (!library.isValid() || !outside.isValid() || !db.isOpen()) {
        check("temporary directories and database", false);
        return;
    }
    Blacklist::setAppliesToLibrary(false);
    DownloadManager downloads(&db);
    downloads.setDownloadDir(library.path());
    QDir(library.path()).mkpath(QStringLiteral("art"));

    LibraryTab tab(&db, &downloads);
    auto *view = tab.findChild<QListView *>(QStringLiteral("libraryView"));
    auto *status = tab.findChild<QLabel *>(QStringLiteral("libraryStatusLabel"));
    auto *button = tab.findChild<QPushButton *>(QStringLiteral("importButton"));
    auto *folders = tab.findChildren<QComboBox *>().value(0);
    if (!view || !status || !button || !folders) {
        check("the tab's widgets are found", false);
        return;
    }
    tab.resize(900, 700);
    tab.show();
    tab.refreshLibraryFolderTreeRoot();
    tab.start();
    check("the import button is there", button->isVisibleTo(&tab) && button->isEnabled());

    // The folder chosen above the grid is where the copies go.
    folders->setCurrentIndex(folders->findData(QStringLiteral("art")));
    const QString one = outside.filePath(QStringLiteral("one.png"));
    writeFile(one, pngOfSize(32, 32, Qt::red));
    bool answered = false;
    answerNextInputDialog(QStringLiteral("cat, sky"), &answered);
    tab.importPaths({one});
    check("the import asks for tags", answered);
    check("the copy is in the chosen folder", QFile::exists(library.filePath(QStringLiteral("art/one.png"))));
    check("and shows up in the grid", view->model()->rowCount() == 1, QString::number(view->model()->rowCount()));
    check("the status says so", status->text().contains(QStringLiteral("Zaimportowano 1 ")), status->text());
    equal("tags are split at spaces and commas", sidecarOf(library.filePath(QStringLiteral("art/one.png"))).join(QLatin1Char(' ')),
          QStringLiteral("cat sky"));

    // Cancelling the question imports nothing.
    const QString two = outside.filePath(QStringLiteral("two.png"));
    writeFile(two, pngOfSize(32, 32, Qt::green));
    QTimer::singleShot(0, []() {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    tab.importPaths({two});
    check("cancelling imports nothing", !QFile::exists(library.filePath(QStringLiteral("art/two.png"))));

    // Dropping a file from the file manager onto the grid.
    auto *mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(two)});
    const QPoint at = view->viewport()->rect().center();
    QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &enter);
    check("the grid accepts dropped files", enter.isAccepted());
    QDropEvent drop(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    answered = false;
    answerNextInputDialog(QString(), &answered);
    QApplication::sendEvent(view->viewport(), &drop);
    QElapsedTimer waited;
    waited.start();
    while (!QFile::exists(library.filePath(QStringLiteral("art/two.png"))) && waited.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    check("a dropped file is imported", answered && QFile::exists(library.filePath(QStringLiteral("art/two.png"))));
    check("and shows up in the grid", view->model()->rowCount() == 2, QString::number(view->model()->rowCount()));
    delete mime;

    // Something that is no file is not taken.
    QMimeData text;
    text.setText(QStringLiteral("just text"));
    QDragEnterEvent textEnter(at, Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &textEnter);
    check("text dragged over the grid is not accepted", !textEnter.isAccepted());
}

} // namespace

int main(int argc, char **argv)
{
    // Redirects settings and app data into a throwaway location.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("blairqt-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("blairqt-tests"));

    // Start from a clean database and source list every run.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile::remove(appData + QStringLiteral("/library.sqlite3"));
    QFile::remove(appData + QStringLiteral("/custom_sources.json"));
    QFile::remove(appData + QStringLiteral("/hidden_sources.json"));

    testHtmlEntities();
    testAppFiles();
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
    testShimmieSource();
    testDownloadExtensionHint();
    testDetectorAddresses();
    testDetectorClues();
    testDetectorEndToEnd();
    testGelbooruHtmlSource();
    testSiteAccess();
    testAnimatedThumbnails();
    testQueryEncoding();
    testTagExclusionLimits();
    testClipboardImage();
    testCookieStaysOnItsSite();
    testDownloadQueue();
    testThumbnailDecoding();
    testThumbnailReload();
    testLibrarySeveralTags();
    testSearchTab();
    testLibraryTab();
    testLibraryImport();
    testLibraryTabImport();
    // Last: it changes the application palette.
    testTheme();

    std::cout << (failures ? "\nFAILED checks: " : "\nall checks passed: ") << failures << "\n";
    return failures == 0 ? 0 : 1;
}

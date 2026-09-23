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

    const QByteArray gelbooruPosts = R"JSON([{"id":1,"tags":"baldur&#039;s_gate_3 1girl",
        "file_url":"https://x/a.jpg","preview_url":"https://x/t.jpg"}])JSON";
    const QVector<Post> posts = gelbooru.parseSearchResponse(gelbooruPosts);
    check("gelbooru post parsed", posts.size() == 1, QString::number(posts.size()));
    if (!posts.isEmpty()) {
        equal("post tags are unescaped", posts.first().tags.join(QLatin1Char(',')),
              QStringLiteral("baldur's_gate_3,1girl"));
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
    testBlacklist();
    testTagInput();
    testSources();
    testViewerUrls();
    testAutoLoadChain();
    testTagExclusionComposition();
    testPluginSource();
    testLibraryDatabase();

    std::cout << (failures ? "\nFAILED checks: " : "\nall checks passed: ") << failures << "\n";
    return failures == 0 ? 0 : 1;
}

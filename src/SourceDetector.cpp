// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SourceDetector.h"
#include "AppInfo.h"
#include "NetworkLog.h"
#include "SourceManager.h"
#include "sources/ISource.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>

namespace {

constexpr int kProbeTimeoutMs = 8000;
constexpr int kProbePosts = 5;
// A front page is read only for clues; its start is enough.
constexpr int kFrontPageBytes = 2 * 1024 * 1024;

// Tried in order: an empty search, then "all" (what Gelbooru-engine forks
// use for "everything"), then a common tag for sites that answer nothing to
// either.
const QStringList &probeTags()
{
    static const QStringList tags = {QString(), QStringLiteral("all"), QStringLiteral("1girl")};
    return tags;
}

// A path segment that leads into a site - a post, a search, the API -
// rather than being part of where the site is installed.
bool leadsIntoSite(const QString &segment)
{
    static const QStringList segments = {
        QStringLiteral("index.php"), QStringLiteral("index.html"), QStringLiteral("posts"),
        QStringLiteral("post"), QStringLiteral("images"), QStringLiteral("image"),
        QStringLiteral("api"), QStringLiteral("search"), QStringLiteral("tags"),
        QStringLiteral("tag"), QStringLiteral("wiki"), QStringLiteral("wiki_pages"),
        QStringLiteral("pools"), QStringLiteral("pool"), QStringLiteral("comment"),
        QStringLiteral("comments"), QStringLiteral("artists"), QStringLiteral("artist"),
        QStringLiteral("favorites"), QStringLiteral("forum"), QStringLiteral("forum_topics"),
        QStringLiteral("user"), QStringLiteral("users"), QStringLiteral("help"),
        QStringLiteral("explore"), QStringLiteral("popular"), QStringLiteral("random"),
    };
    return segments.contains(segment.toLower());
}

// An answer in some engine's own format (JSON or XML), as opposed to an
// HTML page or an error text.
bool looksLikeData(const QByteArray &body)
{
    const QByteArray trimmed = body.trimmed();
    return trimmed.startsWith('[') || trimmed.startsWith('{') || trimmed.startsWith("<?xml")
        || trimmed.startsWith("<posts") || trimmed.startsWith("<rss");
}

QString snippet(const QByteArray &body)
{
    return QString::fromUtf8(body.left(200)).simplified();
}

} // namespace

SourceDetector::SourceDetector(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

QStringList SourceDetector::candidateBaseUrls(const QString &typed)
{
    QString text = typed.trimmed();
    if (text.isEmpty())
        return {};
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("https://"));

    const QUrl url(text, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        return {};
    }

    QString origin = scheme + QStringLiteral("://") + url.host();
    if (url.port() != -1)
        origin += QLatin1Char(':') + QString::number(url.port());

    // Everything up to the first segment that leads into the site, so that
    // a booru installed below the root ("/booru/") keeps its folder.
    QStringList kept;
    for (const QString &segment : url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (leadsIntoSite(segment))
            break;
        kept.append(segment);
    }

    QString base = origin;
    if (!kept.isEmpty())
        base += QLatin1Char('/') + kept.join(QLatin1Char('/'));

    QStringList candidates = {base};
    if (base != origin)
        candidates.append(origin);
    return candidates;
}

SourceDetector::PageClues SourceDetector::readFrontPage(const QByteArray &html)
{
    const QString page = QString::fromUtf8(html.left(kFrontPageBytes)).toLower();
    PageClues clues;

    if (page.contains(QStringLiteral("cf-chl")) || page.contains(QStringLiteral("challenges.cloudflare.com"))
        || page.contains(QStringLiteral("<title>just a moment"))
        || page.contains(QStringLiteral("attention required! | cloudflare"))) {
        clues.blocker = QStringLiteral("Cloudflare");
    } else if (page.contains(QStringLiteral("anubis"))
               && (page.contains(QStringLiteral("within.website"))
                   || page.contains(QStringLiteral("protected by anubis")))) {
        clues.blocker = QStringLiteral("Anubis");
    } else if (page.contains(QStringLiteral("ddos-guard"))) {
        clues.blocker = QStringLiteral("DDoS-Guard");
    }

    // Most specific first: e621 is a Danbooru fork and mentions it, and a
    // "/posts/" link alone could be anything.
    struct Clue {
        QString flavor;
        QStringList marks;
    };
    const QVector<Clue> clueList = {
        {QStringLiteral("e621"), {QStringLiteral("e621"), QStringLiteral("e926"), QStringLiteral("e6ai")}},
        {QStringLiteral("philomena"), {QStringLiteral("philomena"), QStringLiteral("/api/v1/json"),
                                       QStringLiteral("data-image-id")}},
        {QStringLiteral("szurubooru"), {QStringLiteral("szurubooru")}},
        {QStringLiteral("shimmie"), {QStringLiteral("shimmie"), QStringLiteral("/post/view/"),
                                     QStringLiteral("q=post/list"), QStringLiteral("q=post/view")}},
        {QStringLiteral("gelbooru"), {QStringLiteral("index.php?page=post"), QStringLiteral("gelbooru")}},
        {QStringLiteral("moebooru"), {QStringLiteral("/post/show/"), QStringLiteral("moebooru")}},
        {QStringLiteral("danbooru"), {QStringLiteral("danbooru"), QStringLiteral("/posts?tags="),
                                      QStringLiteral("/posts/")}},
    };
    for (const Clue &clue : clueList) {
        for (const QString &mark : clue.marks) {
            if (page.contains(mark)) {
                clues.flavors.append(clue.flavor);
                break;
            }
        }
    }

    clues.gelbooruListing = page.contains(QStringLiteral("page=post&s=list"))
        || page.contains(QStringLiteral("page=post&amp;s=list"))
        || page.contains(QStringLiteral("s=view&id=")) || page.contains(QStringLiteral("s=view&amp;id="));
    return clues;
}

bool SourceDetector::asksForCredentials(int httpStatus, const QByteArray &body)
{
    if (httpStatus == 401)
        return true;
    const QString text = QString::fromUtf8(body.left(4096)).toLower();
    // "Missing authentication. Go to api.rule34.xxx ..." and the like.
    return text.contains(QStringLiteral("missing authentication"))
        || text.contains(QStringLiteral("authentication required"))
        || text.contains(QStringLiteral("api_key")) || text.contains(QStringLiteral("api key"))
        || text.contains(QStringLiteral("login required"))
        || text.contains(QStringLiteral("you must be logged in"));
}

void SourceDetector::start(const Request &request)
{
    ++m_generation;
    m_request = request;
    m_blocker.clear();
    m_credentialsFlavor.clear();
    m_gelbooruListing = false;
    m_candidateIndex = 0;

    // A plugin decides itself what its base address means.
    if (request.flavor == QStringLiteral("python")) {
        QString url = request.url.trimmed();
        while (url.endsWith(QLatin1Char('/')))
            url.chop(1);
        m_candidates = url.isEmpty() ? QStringList() : QStringList{url};
    } else {
        m_candidates = candidateBaseUrls(request.url);
    }

    if (m_candidates.isEmpty()) {
        Result result;
        result.failure = Failure::InvalidUrl;
        finishWith(result);
        return;
    }
    startCandidate();
}

QNetworkRequest SourceDetector::withUserAgent(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      m_request.userAgent.isEmpty() ? blairqtUserAgent() : m_request.userAgent);
    if (!m_request.cookie.isEmpty())
        request.setRawHeader("Cookie", m_request.cookie.toUtf8());
    request.setTransferTimeout(kProbeTimeoutMs);
    return request;
}

void SourceDetector::startCandidate()
{
    m_base = m_candidates.at(m_candidateIndex);

    QStringList flavors;
    if (m_request.flavor.isEmpty()) {
        flavors = SourceManager::knownFlavors();
        if (!m_request.scriptPath.isEmpty())
            flavors.append(QStringLiteral("python"));
    } else {
        flavors = QStringList{m_request.flavor};
    }
    m_order = flavors;

    // The front page only helps guess among engines; with one engine chosen
    // or the HTML bridge doing the fetching, there is nothing to guess.
    if (!m_request.flavor.isEmpty() || !m_request.htmlBridgeUrl.isEmpty()) {
        m_orderIndex = 0;
        m_pass = 0;
        m_retry.clear();
        probeNext();
        return;
    }

    emit logLine(QStringLiteral("GET %1/ (front page)").arg(m_base));
    QNetworkReply *reply = m_network->get(withUserAgent(QUrl(m_base + QLatin1Char('/'))));
    const int generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        reply->deleteLater();
        if (generation == m_generation)
            onFrontPageFinished(reply);
    });
}

void SourceDetector::onFrontPageFinished(QNetworkReply *reply)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Read on errors too: a 403 or 503 page is where a blocker shows itself.
    const QByteArray body = reply->readAll();
    emit logLine(QStringLiteral("  -> HTTP %1, %2 bytes%3").arg(httpStatus).arg(body.size())
        .arg(reply->error() == QNetworkReply::NoError ? QString() : QStringLiteral(" | ") + reply->errorString()));

    // A redirect to https or to another host name ("www.") is where the
    // site really is. Only the scheme and host are taken over: a redirect
    // to a login page or a post list says nothing about the base path.
    const QUrl original(m_base);
    const QUrl landed = reply->url();
    if (landed.isValid() && !landed.host().isEmpty()
        && (landed.scheme() != original.scheme() || landed.host() != original.host()
            || landed.port() != original.port())) {
        QString moved = landed.scheme() + QStringLiteral("://") + landed.host();
        if (landed.port() != -1)
            moved += QLatin1Char(':') + QString::number(landed.port());
        moved += original.path();
        while (moved.endsWith(QLatin1Char('/')))
            moved.chop(1);
        emit logLine(QStringLiteral("  -> redirected; using %1").arg(moved));
        m_base = moved;
    }

    const PageClues clues = readFrontPage(body);
    if (!clues.blocker.isEmpty()) {
        m_blocker = clues.blocker;
        emit logLine(QStringLiteral("  -> bot protection page: %1").arg(clues.blocker));
    }
    if (clues.gelbooruListing)
        m_gelbooruListing = true;

    // Likely engines first, the rest after them in the usual order.
    QStringList order;
    for (const QString &flavor : clues.flavors) {
        if (m_order.contains(flavor))
            order.append(flavor);
    }
    for (const QString &flavor : std::as_const(m_order)) {
        if (!order.contains(flavor))
            order.append(flavor);
    }
    if (!clues.flavors.isEmpty())
        emit logLine(QStringLiteral("  -> looks like: %1").arg(clues.flavors.join(QStringLiteral(", "))));
    m_order = order;

    m_orderIndex = 0;
    m_pass = 0;
    m_retry.clear();
    probeNext();
}

void SourceDetector::probeNext()
{
    const QStringList &list = m_pass == 0 ? m_order : m_retry;
    if (m_orderIndex >= list.size()) {
        if (m_pass + 1 < probeTags().size() && !m_retry.isEmpty()) {
            ++m_pass;
            m_orderIndex = 0;
            emit logLine(QStringLiteral("(no posts for that search - asking again with tag \"%1\")")
                             .arg(probeTags().at(m_pass)));
            probeNext();
            return;
        }
        ++m_candidateIndex;
        if (m_candidateIndex < m_candidates.size()) {
            emit logLine(QStringLiteral("(nothing at %1 - trying %2)").arg(m_base, m_candidates.at(m_candidateIndex)));
            startCandidate();
            return;
        }
        finishWith(Result());
        return;
    }

    const QString flavor = list.at(m_orderIndex);
    m_probeSource = SourceManager::createSource(QStringLiteral("__probe__"), flavor, m_base,
                                                 m_request.apiKey, m_request.userId, m_request.cookie,
                                                 m_request.userAgent, m_request.htmlBridgeUrl,
                                                 m_request.scriptPath);
    if (!m_probeSource) {
        ++m_orderIndex;
        probeNext();
        return;
    }

    const QString tag = probeTags().at(m_pass);
    emit probing(flavor, m_orderIndex + 1, list.size(), tag);

    const QSharedPointer<ISource> source = m_probeSource;
    if (!source->mayBlock()) {
        sendProbe(source->buildSearchRequest(tag, 1, kProbePosts), flavor);
        return;
    }

    // A plugin builds its URL by running Python; never on the GUI thread.
    auto *watcher = new QFutureWatcher<QNetworkRequest>(this);
    const int generation = m_generation;
    connect(watcher, &QFutureWatcher<QNetworkRequest>::finished, this, [this, watcher, flavor, generation]() {
        watcher->deleteLater();
        if (generation == m_generation)
            sendProbe(watcher->result(), flavor);
    });
    watcher->setFuture(QtConcurrent::run([source, tag]() {
        return source->buildSearchRequest(tag, 1, kProbePosts);
    }));
}

void SourceDetector::sendProbe(QNetworkRequest request, const QString &flavor)
{
    if (!request.url().isValid() || request.url().isEmpty()) {
        emit logLine(QStringLiteral("[%1] no usable request was built").arg(flavor));
        ++m_orderIndex;
        probeNext();
        return;
    }

    request.setTransferTimeout(kProbeTimeoutMs);
    const QString shownUrl = NetworkLog::redactedUrl(request.url());
    emit logLine(QStringLiteral("[%1] GET %2").arg(flavor, shownUrl));
    NetworkLog::instance().log(QStringLiteral("(source test) GET %1").arg(shownUrl));

    QNetworkReply *reply = m_network->get(request);
    const int generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, flavor, generation]() {
        reply->deleteLater();
        if (generation == m_generation)
            onProbeFinished(reply, flavor);
    });
}

void SourceDetector::onProbeFinished(QNetworkReply *reply, const QString &flavor)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        emit logLine(QStringLiteral("  -> %1").arg(reply->errorString()));
        onProbeParsed(flavor, httpStatus, body, {});
        return;
    }

    const QSharedPointer<ISource> source = m_probeSource;
    if (!source->mayBlock()) {
        onProbeParsed(flavor, httpStatus, body, source->parseSearchResponse(body));
        return;
    }

    auto *watcher = new QFutureWatcher<QVector<Post>>(this);
    const int generation = m_generation;
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
            [this, watcher, flavor, httpStatus, body, generation]() {
        watcher->deleteLater();
        if (generation == m_generation)
            onProbeParsed(flavor, httpStatus, body, watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([source, body]() {
        return source->parseSearchResponse(body);
    }));
}

void SourceDetector::onProbeParsed(const QString &flavor, int httpStatus, const QByteArray &body,
                                   const QVector<Post> &posts)
{
    emit logLine(QStringLiteral("  -> HTTP %1, %2 posts parsed%3").arg(httpStatus).arg(posts.size())
        .arg(posts.isEmpty() && !body.isEmpty() ? QStringLiteral(" | ") + snippet(body) : QString()));

    if (!posts.isEmpty()) {
        Result result;
        result.found = true;
        result.flavor = flavor;
        result.baseUrl = m_base;
        result.postCount = int(posts.size());
        finishWith(result);
        return;
    }

    // An API path behind the same protection as the front page.
    const PageClues clues = readFrontPage(body);
    if (!clues.blocker.isEmpty() && m_blocker.isEmpty())
        m_blocker = clues.blocker;
    if (m_credentialsFlavor.isEmpty() && asksForCredentials(httpStatus, body))
        m_credentialsFlavor = flavor;

    // Worth asking again with a tag only if it answered in a format of its
    // own; a plugin's site may answer with an HTML page it scrapes.
    if (m_pass == 0 && httpStatus == 200 && (looksLikeData(body) || flavor == QStringLiteral("python")))
        m_retry.append(flavor);

    ++m_orderIndex;
    probeNext();
}

void SourceDetector::finishWith(Result result)
{
    if (!result.found && result.failure == Failure::None) {
        if (!m_blocker.isEmpty()) {
            result.failure = Failure::Blocked;
            result.blocker = m_blocker;
        } else if (!m_credentialsFlavor.isEmpty()) {
            result.failure = Failure::NeedsCredentials;
            result.credentialsFlavor = m_credentialsFlavor;
        } else if (m_gelbooruListing && m_request.htmlBridgeUrl.isEmpty()) {
            result.failure = Failure::NeedsHtmlBridge;
        } else {
            result.failure = Failure::NothingMatched;
        }
    }
    ++m_generation; // nothing of this detection may arrive after its end
    emit finished(result);
}

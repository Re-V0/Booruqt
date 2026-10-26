// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "Post.h"

class QNetworkAccessManager;
class QNetworkReply;
class ISource;

// Works out which engine runs a booru, starting from whatever address the
// user pasted: the front page, a post, a search, with or without https://.
//
// 1. The address is cut back to the site's base: "https://" is added when
//    missing, and a path into the site (a post, a search, index.php) is
//    dropped. The bare host is tried as well when the site lives below it.
// 2. The front page is fetched once. Links and names on it suggest the
//    engine, so that one is probed first, and it shows a bot protection
//    page (Cloudflare, Anubis, DDoS-Guard) for what it is. A redirect, to
//    https or to www., moves the base along.
// 3. Each engine's search API is asked for a few posts. The first that
//    answers with posts wins. One that answers in its format but with no
//    posts is asked again with a tag, since some engines return nothing for
//    an empty search.
//
// No message here is translated: the caller turns the facts it reports
// (which engine, which tag, why nothing matched) into text.
class SourceDetector : public QObject {
    Q_OBJECT
public:
    // Everything a probe source is built from.
    struct Request {
        QString url;      // as the user typed it
        QString flavor;   // empty: try every known engine
        QString apiKey;
        QString userId;
        QString cookie;
        QString userAgent;
        QString htmlBridgeUrl;
        QString scriptPath;
    };

    // Why a detection found nothing, most useful explanation first.
    enum class Failure {
        None,
        InvalidUrl,
        // The front page was a bot protection page; blocker names it.
        Blocked,
        // An engine answered, but asked for an API key or a login.
        NeedsCredentials,
        // A Gelbooru-engine site whose API is off: the HTML bridge can read it.
        NeedsHtmlBridge,
        NothingMatched,
    };

    struct Result {
        bool found = false;
        QString flavor;
        // Normalized; what the source should be saved with.
        QString baseUrl;
        int postCount = 0;
        Failure failure = Failure::None;
        QString blocker;
        // For NeedsCredentials: the engine that asked.
        QString credentialsFlavor;
    };

    // What a front page gives away.
    struct PageClues {
        // Likely engines, most specific first.
        QStringList flavors;
        // "Cloudflare", "Anubis", "DDoS-Guard", or empty.
        QString blocker;
        // It lists posts the way Gelbooru does (index.php?page=post&s=list).
        bool gelbooruListing = false;
    };

    explicit SourceDetector(QObject *parent = nullptr);

    // Starts a detection; finished() reports the end. Starting again while
    // one runs abandons the first.
    void start(const Request &request);

    // The base addresses to try for what the user typed, best first. Empty
    // when it cannot be a web address.
    static QStringList candidateBaseUrls(const QString &typed);
    static PageClues readFrontPage(const QByteArray &html);
    // Whether a probe answer refuses for lack of an API key or login.
    static bool asksForCredentials(int httpStatus, const QByteArray &body);

signals:
    // Before each probe request. tag is empty for the plain empty search.
    void probing(const QString &flavor, int index, int total, const QString &tag);
    // One line per request and answer, for a log view. English, like the
    // network log.
    void logLine(const QString &line);
    void finished(const SourceDetector::Result &result);

private:
    void startCandidate();
    void onFrontPageFinished(QNetworkReply *reply);
    void probeNext();
    void sendProbe(QNetworkRequest request, const QString &flavor);
    void onProbeFinished(QNetworkReply *reply, const QString &flavor);
    void onProbeParsed(const QString &flavor, int httpStatus, const QByteArray &body,
                       const QVector<Post> &posts);
    void finishWith(Result result);
    QNetworkRequest withUserAgent(const QUrl &url) const;

    QNetworkAccessManager *m_network;
    // Bumped by start(); answers of an abandoned detection are ignored.
    int m_generation = 0;

    Request m_request;
    QStringList m_candidates;
    int m_candidateIndex = 0;
    QString m_base;

    // The engines to probe for the current base, in order, and for each
    // pass after the first only those that answered in their own format.
    QStringList m_order;
    QStringList m_retry;
    int m_orderIndex = 0;
    int m_pass = 0;
    QSharedPointer<ISource> m_probeSource;

    // Collected over all candidates, to explain a failure.
    QString m_blocker;
    QString m_credentialsFlavor;
    bool m_gelbooruListing = false;
};

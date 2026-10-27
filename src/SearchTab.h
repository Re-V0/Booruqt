// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QWidget>
#include <QModelIndex>
#include <QPixmap>
#include <QPoint>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QNetworkRequest>

#include "Post.h"

class QComboBox;
class QPushButton;
class QListView;
class QSlider;
class QSpinBox;
class QCheckBox;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QCompleter;
class QStringListModel;
class ISource;
class SourceManager;
class LibraryDatabase;
class DownloadManager;
class GalleryModel;
class ThumbnailLoader;
class AnimatedThumbnails;
class TagLineEdit;

// The "Wyszukiwanie" tab: picks the sources, searches them, and shows the
// results with paging, infinite scroll and autoscroll.
//
// It owns its gallery, its thumbnails and its network requests. The sources,
// the library and the downloads belong to MainWindow, which outlives the tab.
class SearchTab : public QWidget {
    Q_OBJECT
public:
    SearchTab(SourceManager &sourceManager, LibraryDatabase *library,
              DownloadManager *downloadManager, QWidget *parent = nullptr);

    // Selects the saved startup sources and runs the first search. On the
    // first run it asks for them in the welcome dialog instead.
    void start();

    // Moves the thumbnail size slider as if the user had; a value outside
    // its range is ignored.
    void setThumbnailSliderValue(int value);
    // Shows a size chosen on either tab. Emits nothing.
    void applyThumbnailSize(int value);
    // Video and GIF posts move in the grid (see AnimatedThumbnails).
    void setAnimatedThumbnails(bool enabled);

public slots:
    // Does nothing while no source is selected.
    void startNewSearch();
    void chooseStartupSources();
    void restoreDefaultSources();
    void chooseResultsPerPage();

signals:
    // Text for the main window's status bar.
    void statusMessage(const QString &text);
    // The user moved this tab's thumbnail size slider.
    void thumbnailSizeChanged(int value);
    void postClicked(const Post &post, const QPixmap &preview);

protected:
    // Lets Enter start a search instead of being eaten by the suggestion list.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void loadNextPage();
    void onPrevPage();
    void onNextPage();
    void onPageSpinEditingFinished();
    void onThumbnailReady(int row, QPixmap pixmap);
    void requestVisibleThumbnails();
    void maybeAutoLoadNextPage();
    void onSourceComboChanged(int index);
    void chooseMultipleSources();
    void deleteCurrentSource();
    void toggleAutoscroll(bool enabled);
    void autoscrollTick();
    void downloadSelected();
    void showGalleryContextMenu(const QPoint &pos);
    void onGalleryClicked(const QModelIndex &index);

    void onTagsEdited();
    void fetchTagSuggestions();
    void onTagSuggestReplyFinished();

private:
    void buildUi();
    void setupTagCompleter();
    void issueSearchRequest(int page, bool append);
    void sendSearchRequest(const QSharedPointer<ISource> &source, QNetworkRequest request);
    void onSearchReplyFinished(QNetworkReply *reply, QSharedPointer<ISource> source);
    void handleSearchPosts(const QSharedPointer<ISource> &source, const QVector<Post> &parsed,
                           int httpStatus, const QByteArray &data);
    // A round ends once no request is still being built and none is in flight.
    void finishSearchRoundIfDone();
    // Cancels the in-flight search replies; their finished() is then ignored.
    void abortPendingSearchReplies();
    void populateSourceCombo(const QString &selectName = {});
    void updateSourceSelectionUi();
    void updateDownloadButtonLabel();
    // Page controls make no sense while the view keeps appending.
    void updatePagingControlsEnabled();
    void runStartupSourcesDialog(bool firstRun);

    // Not owned.
    SourceManager &m_sourceManager;
    LibraryDatabase *m_library;

    GalleryModel *m_model;
    ThumbnailLoader *m_thumbnailLoader;
    AnimatedThumbnails *m_animations = nullptr;
    // Not owned.
    DownloadManager *m_downloadManager;
    QNetworkAccessManager *m_network;

    QComboBox *m_sourceCombo;
    QPushButton *m_multiSourceButton;
    QPushButton *m_deleteSourceButton;
    TagLineEdit *m_tagsEdit;
    QPushButton *m_searchButton;
    QPushButton *m_loadMoreButton;
    QListView *m_galleryView;
    QSlider *m_sizeSlider;

    QPushButton *m_downloadButton;

    // Paging and infinite scroll are independent, not mutually exclusive.
    QSpinBox *m_pageSpin;
    QPushButton *m_prevPageButton;
    QPushButton *m_nextPageButton;
    QCheckBox *m_infiniteScrollCheck;

    // Slideshow-style auto-scroll; auto-loads more pages, stops on empty.
    QCheckBox *m_autoscrollCheck;
    QSlider *m_autoscrollSpeedSlider;
    QTimer *m_autoscrollTimer;

    // Tag autocomplete from the active source.
    QCompleter *m_tagCompleter = nullptr;
    QStringListModel *m_tagSuggestModel = nullptr;
    QTimer *m_tagSuggestTimer = nullptr;
    QNetworkReply *m_tagSuggestReply = nullptr;
    QSharedPointer<ISource> m_tagSuggestSource;
    QString m_tagSuggestPrefix;

    // Sources for the current search; >1 means a merged search.
    QStringList m_selectedMergeSources;
    QString m_currentTags;
    int m_currentPage = 1;
    int m_lastPageResultCount = -1;
    bool m_loading = false;
    QSet<QNetworkReply *> m_pendingReplies;
    // Work handed to worker threads for blocking sources (plugins).
    int m_pendingBackgroundJobs = 0;
    // Bumped per search round; stale worker results check it and bail out.
    int m_searchRound = 0;
    int m_roundResultCount = 0;
    // Posts dropped by the blacklist in the current round.
    int m_blockedThisRound = 0;
    // Consecutive pages infinite scroll pulled without a scroll event.
    int m_autoLoadChain = 0;
    // Posts the sources returned before filtering; decides "end of results".
    int m_rawRoundCount = 0;
    // Configurable in Settings; persisted in QSettings.
    int m_resultsPerPage = 40;
};

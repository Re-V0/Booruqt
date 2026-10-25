// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QMainWindow>
#include <QSet>
#include <QHash>
#include <memory>

class QComboBox;
class QLineEdit;
class QPushButton;
class QListView;
class QSlider;
class QLabel;
class QSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QMenu;
class QPoint;
class QModelIndex;
class QDockWidget;
class QCloseEvent;
class QTreeView;
class QFileSystemModel;
class QListWidget;
class QCompleter;
class QStringListModel;
class QAction;
class ISource;
template <typename T> class QSharedPointer;

#include "GalleryModel.h"
#include "ThumbnailLoader.h"
#include "SourceManager.h"
#include "DownloadManager.h"
#include "LibraryDatabase.h"
#include "DetailsPanel.h"
#include "TagLineEdit.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // System UI language if translated, else English.
    static QString defaultLanguageCode();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Lets Enter start a search instead of being eaten by the suggestion list.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void startNewSearch();
    void loadNextPage();
    void onPrevPage();
    void onNextPage();
    void onPageSpinEditingFinished();
    void onThumbnailReady(int row, QPixmap pixmap);
    void onIconSizeChanged(int value);
    void requestVisibleThumbnails();
    void maybeAutoLoadNextPage();
    void onSourceComboChanged(int index);
    void chooseMultipleSources();
    void deleteCurrentSource();
    void restoreDefaultSources();
    void toggleAutoscroll(bool enabled);
    void autoscrollTick();
    void downloadSelected();
    void chooseDownloadDir();
    void chooseResultsPerPage();
    void chooseStartupSources();
    void manageBlacklist();
    void manageAllowedFolders();
    void reloadTheme();
    void openThemeFile();
    void pickCuratedTheme();
    void showGalleryContextMenu(const QPoint &pos);
    void onGalleryClicked(const QModelIndex &index);

    void searchLibrary();
    void loadMoreLibrary();
    void onLibraryThumbnailReady(int row, QPixmap pixmap);
    void requestVisibleLibraryThumbnails();
    void onLibraryClicked(const QModelIndex &index);
    void showLibraryContextMenu(const QPoint &pos);
    void deleteSelectedLibraryItems();
    void onLibraryFolderChanged(int index);
    void onLibraryFolderTreeSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void toggleLibraryTreeView(bool enabled);
    void createNewLibraryFolder();
    void deleteCurrentLibraryFolder();
    void moveSelectedLibraryItems(const QString &targetFolderName);

    void onZoomRequested(const QString &imageUrl, const QString &title);
    void onDetailsDownloadRequested(const Post &post);

    void changeLanguage(const QString &langCode);

    void onTagsEdited();
    void fetchTagSuggestions();
    void onTagSuggestReplyFinished();
    void onLibraryTagsEdited();

private:
    void buildUi();
    QWidget *buildSearchTab();
    QWidget *buildLibraryTab();
    void buildLanguageMenu(QMenu *settingsMenu);
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
    void fetchLibraryPage(bool append);
    void populateLibraryFolders();
    QStringList libraryFolderNames() const;
    QString currentLibraryFolderSelection() const;
    void updateLibraryDeleteFolderButtonState();
    void refreshLibraryFolderTreeRoot();
    void applyThemeIconSize();
    void showPostInDetailsPanel(const Post &post, const QPixmap &preview);
    void runStartupSourcesDialog(bool firstRun);
    // Moves indexed files and subfolders; returns a status message.
    QString relocateLibrary(const QString &oldDir, const QString &newDir, const QVector<Post> &posts);
    void saveWindowLayout();
    void setupTagCompleters();
    void showTagSuggestions(QCompleter *completer, const QStringList &tags, const QString &prefix);

    SourceManager m_sourceManager;
    std::unique_ptr<LibraryDatabase> m_library;
    GalleryModel *m_model;
    ThumbnailLoader *m_thumbnailLoader;
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
    QLabel *m_statusLabel;
    QPlainTextEdit *m_logView;

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

    // Library tab: browses downloaded posts via the same gallery machinery.
    GalleryModel *m_libraryModel;
    ThumbnailLoader *m_libraryThumbnailLoader = nullptr;
    QListView *m_libraryView = nullptr;
    TagLineEdit *m_libraryTagsEdit;
    QPushButton *m_librarySearchButton;
    QPushButton *m_libraryLoadMoreButton;
    // Second view of the same setting, kept in sync with the search tab's.
    QSlider *m_librarySizeSlider = nullptr;
    QLabel *m_libraryStatusLabel;
    // Paging cursor: rows below this database id are still unread. 0 = start.
    qint64 m_libraryLastId = 0;

    // Real nested folders on disk. Combo data: * = all, empty = root, else path.
    QComboBox *m_libraryFolderCombo;
    QPushButton *m_newFolderButton;
    QPushButton *m_deleteFolderButton;

    // Optional tree view, alternative to the flat combo above.
    QCheckBox *m_libraryTreeViewCheck;
    QCheckBox *m_libraryIncludeSubfoldersCheck;
    QWidget *m_libraryTreePane;
    QListWidget *m_libraryRootFolderList;
    QTreeView *m_libraryFolderTree;
    QFileSystemModel *m_libraryFsModel;

    // Keyed by language code, so a declined restart can restore the tick.
    QHash<QString, QAction *> m_languageActions;

    // Details panel for the last-clicked post.
    DetailsPanel *m_detailsPanel;
    QDockWidget *m_detailsDock;

    // Tag autocomplete: remote for the active source, local for the library.
    QCompleter *m_tagCompleter = nullptr;
    QStringListModel *m_tagSuggestModel = nullptr;
    QCompleter *m_libraryTagCompleter = nullptr;
    QStringListModel *m_libraryTagSuggestModel = nullptr;
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

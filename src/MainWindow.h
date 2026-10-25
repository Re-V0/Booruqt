// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QMainWindow>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <memory>

#include "Post.h"
#include "SourceManager.h"
#include "LibraryDatabase.h"

class QLabel;
class QPlainTextEdit;
class QMenu;
class QDockWidget;
class QCloseEvent;
class QAction;
class DownloadManager;
class DetailsPanel;
class SearchTab;
class LibraryTab;

// The window around the two tabs: the settings menu, the status bar, the
// network log and details docks, themes and the interface language.
//
// It owns what both tabs use - the sources, the library database and the
// download manager - and passes them to the tabs, which it outlives.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // System UI language if translated, else English.
    static QString defaultLanguageCode();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void manageBlacklist();
    void manageAllowedFolders();
    void reloadTheme();
    void openThemeFile();
    void pickCuratedTheme();

    void onZoomRequested(const QString &imageUrl, const QString &title);
    void onDetailsDownloadRequested(const Post &post);

    void changeLanguage(const QString &langCode);

    // Both tabs show the thumbnail size; a change on either applies to both.
    void onThumbnailSizeChanged(int value);

private:
    void buildUi();
    void buildLanguageMenu(QMenu *settingsMenu);
    void applyThemeIconSize();
    void showPostInDetailsPanel(const Post &post, const QPixmap &preview);
    void saveWindowLayout();

    SourceManager m_sourceManager;
    std::unique_ptr<LibraryDatabase> m_library;
    DownloadManager *m_downloadManager;

    SearchTab *m_searchTab;
    LibraryTab *m_libraryTab;

    QLabel *m_statusLabel;
    QPlainTextEdit *m_logView;

    // Keyed by language code, so a declined restart can restore the tick.
    QHash<QString, QAction *> m_languageActions;

    // Details panel for the last-clicked post.
    DetailsPanel *m_detailsPanel;
    QDockWidget *m_detailsDock;
};

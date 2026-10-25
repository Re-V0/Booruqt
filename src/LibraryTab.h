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
#include <QString>
#include <QStringList>
#include <QVector>

#include "Post.h"

class QComboBox;
class QPushButton;
class QListView;
class QSlider;
class QLabel;
class QCheckBox;
class QTreeView;
class QFileSystemModel;
class QListWidget;
class QCompleter;
class QStringListModel;
class LibraryDatabase;
class DownloadManager;
class GalleryModel;
class ThumbnailLoader;
class TagLineEdit;

// The "Biblioteka" tab: browses the downloaded posts by tag and by folder,
// and moves, deletes and relocates their files.
//
// The library database and the download manager belong to MainWindow, which
// outlives the tab.
class LibraryTab : public QWidget {
    Q_OBJECT
public:
    LibraryTab(LibraryDatabase *library, DownloadManager *downloadManager, QWidget *parent = nullptr);

    // Lists the folders and reads the first page of the library.
    void start();

    // Points the folder tree at the current download folder.
    void refreshLibraryFolderTreeRoot();

    // Shows a size chosen on either tab. Emits nothing.
    void applyThumbnailSize(int value);

public slots:
    void searchLibrary();
    // Asks for a new download folder and offers to move the library there.
    void chooseDownloadDir();

signals:
    // The user moved this tab's thumbnail size slider.
    void thumbnailSizeChanged(int value);
    void postClicked(const Post &post, const QPixmap &preview);
    // Posts were deleted, so the details panel may show one that is gone.
    void postsDeleted();

protected:
    // Lets Enter start a search instead of being eaten by the suggestion list.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
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

    void onLibraryTagsEdited();

private:
    void buildUi();
    void setupTagCompleter();
    void fetchLibraryPage(bool append);
    void populateLibraryFolders();
    QStringList libraryFolderNames() const;
    QString currentLibraryFolderSelection() const;
    void updateLibraryDeleteFolderButtonState();
    // Moves indexed files and subfolders; returns a status message.
    QString relocateLibrary(const QString &oldDir, const QString &newDir, const QVector<Post> &posts);

    // Not owned.
    LibraryDatabase *m_library;
    DownloadManager *m_downloadManager;

    // Browses downloaded posts via the same gallery machinery as the search tab.
    GalleryModel *m_libraryModel = nullptr;
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

    // Tag autocomplete from the already-downloaded tags.
    QCompleter *m_libraryTagCompleter = nullptr;
    QStringListModel *m_libraryTagSuggestModel = nullptr;
};

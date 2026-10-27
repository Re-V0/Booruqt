// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "LibraryTab.h"
#include "Blacklist.h"
#include "DownloadManager.h"
#include "FolderAccessGuard.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "LibraryDatabase.h"
#include "Paging.h"
#include "TagLineEdit.h"
#include "ThumbnailLoader.h"
#include "AnimatedThumbnails.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStringListModel>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kLibraryPageSize = 300;

// rename() fails across filesystems; falls back to copy + remove.
bool moveFile(const QString &from, const QString &to)
{
    if (QFile::rename(from, to))
        return true;
    if (!QFile::copy(from, to))
        return false;
    if (QFile::remove(from))
        return true;
    QFile::remove(to);
    return false;
}

} // namespace

LibraryTab::LibraryTab(LibraryDatabase *library, DownloadManager *downloadManager, QWidget *parent)
    : QWidget(parent),
      m_library(library),
      m_downloadManager(downloadManager)
{
    buildUi();
    setupTagCompleter();
    m_libraryAnimations = new AnimatedThumbnails(m_libraryView, m_libraryModel, this);

    // The loader starts at the default cell; a slider change resizes it.
    m_libraryThumbnailLoader->setTargetSize(GalleryView::thumbnailSizeForCell(
        QSize(GalleryView::kDefaultThumbnailSize, GalleryView::kDefaultThumbnailSize)));

    connect(m_libraryThumbnailLoader, &ThumbnailLoader::thumbnailReady, this, &LibraryTab::onLibraryThumbnailReady);
    connect(m_libraryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &LibraryTab::requestVisibleLibraryThumbnails);
    connect(m_libraryView, &QListView::customContextMenuRequested,
            this, &LibraryTab::showLibraryContextMenu);
    connect(m_libraryView, &QListView::clicked, this, &LibraryTab::onLibraryClicked);
    connect(m_librarySearchButton, &QPushButton::clicked, this, &LibraryTab::searchLibrary);
    connect(m_libraryLoadMoreButton, &QPushButton::clicked, this, &LibraryTab::loadMoreLibrary);
    connect(m_libraryTagsEdit, &QLineEdit::returnPressed, this, &LibraryTab::searchLibrary);
    connect(m_libraryFolderCombo, &QComboBox::currentIndexChanged, this, &LibraryTab::onLibraryFolderChanged);
    connect(m_newFolderButton, &QPushButton::clicked, this, &LibraryTab::createNewLibraryFolder);
    connect(m_deleteFolderButton, &QPushButton::clicked, this, &LibraryTab::deleteCurrentLibraryFolder);
    connect(m_libraryTreeViewCheck, &QCheckBox::toggled, this, &LibraryTab::toggleLibraryTreeView);
    connect(m_libraryIncludeSubfoldersCheck, &QCheckBox::toggled, this, &LibraryTab::searchLibrary);
    connect(m_libraryFolderTree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &LibraryTab::onLibraryFolderTreeSelectionChanged);
    connect(m_libraryRootFolderList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row != 0)
            return; // only one row exists; ignore deselection (-1)
        m_libraryFolderTree->clearSelection();
        m_libraryFolderTree->setCurrentIndex(QModelIndex());
        updateLibraryDeleteFolderButtonState();
        searchLibrary();
    });

    // Delete key removes selected library items, scoped to focus.
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_libraryView);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &LibraryTab::deleteSelectedLibraryItems);
}

void LibraryTab::start()
{
    populateLibraryFolders();
    searchLibrary();
}

void LibraryTab::applyThumbnailSize(int value)
{
    const QSize size(value, value);
    if (m_librarySizeSlider->value() != value) {
        const QSignalBlocker blocker(m_librarySizeSlider);
        m_librarySizeSlider->setValue(value);
    }
    m_libraryThumbnailLoader->setTargetSize(GalleryView::thumbnailSizeForCell(size));
    GalleryView::setCellSize(m_libraryView, size);
    requestVisibleLibraryThumbnails();
}

void LibraryTab::setupTagCompleter()
{
    // setWidget() instead of QLineEdit::setCompleter(): only the tag being
    // typed gets replaced, not the whole line. Arrow keys work in the popup.
    m_libraryTagSuggestModel = new QStringListModel(this);
    m_libraryTagCompleter = new QCompleter(m_libraryTagSuggestModel, this);
    m_libraryTagCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_libraryTagCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_libraryTagCompleter->setWidget(m_libraryTagsEdit);
    connect(m_libraryTagCompleter, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &tag) { m_libraryTagsEdit->acceptCompletion(tag); });
    connect(m_libraryTagsEdit, &QLineEdit::textEdited, this, &LibraryTab::onLibraryTagsEdited);

    // Installed after QCompleter's own filter, so this one sees keys first.
    m_libraryTagCompleter->popup()->installEventFilter(this);
}

bool LibraryTab::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // With no row highlighted the list has nothing to accept, so the
            // keypress belongs to the search rather than to the completer.
            if (watched == m_libraryTagCompleter->popup()
                && !m_libraryTagCompleter->popup()->currentIndex().isValid()) {
                m_libraryTagCompleter->popup()->hide();
                m_libraryTagsEdit->clearSuggestion();
                searchLibrary();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibraryTab::onLibraryTagsEdited()
{
    const QString token = m_libraryTagsEdit->currentToken();
    if (token.size() < 2 || !m_library) {
        m_libraryTagCompleter->popup()->hide();
        return;
    }
    showTagSuggestions(m_libraryTagCompleter, m_library->suggestTags(token, 10), token);
}

void LibraryTab::buildUi()
{
    m_libraryModel = new GalleryModel(this);
    // Library thumbnails are local files: nothing to cache.
    m_libraryThumbnailLoader = new ThumbnailLoader(this, /*useDiskCache=*/false);

    auto *rootLayout = new QVBoxLayout(this);

    auto *folderBar = new QHBoxLayout();
    folderBar->addWidget(new QLabel(tr("Folder:"), this));
    m_libraryFolderCombo = new QComboBox(this);
    m_libraryTreeViewCheck = new QCheckBox(tr("Widok drzewa"), this);
    // Lives inside the tree pane: it only ever applied to tree selections.
    m_libraryIncludeSubfoldersCheck = new QCheckBox(tr("z podfolderami"), this);
    m_newFolderButton = new QPushButton(tr("Nowy folder..."), this);
    m_deleteFolderButton = new QPushButton(tr("Usuń folder..."), this);
    folderBar->addWidget(m_libraryFolderCombo, 1);
    folderBar->addWidget(m_libraryTreeViewCheck);
    folderBar->addWidget(m_newFolderButton);
    folderBar->addWidget(m_deleteFolderButton);
    rootLayout->addLayout(folderBar);

    auto *searchBar = new QHBoxLayout();
    m_libraryTagsEdit = new TagLineEdit(this);
    m_libraryTagsEdit->setPlaceholderText(tr("filtruj po tagach (puste = wszystko)..."));
    m_librarySearchButton = new QPushButton(tr("Szukaj w bibliotece"), this);
    m_libraryLoadMoreButton = new QPushButton(tr("Wczytaj więcej"), this);
    m_libraryLoadMoreButton->setEnabled(false);
    m_librarySizeSlider = new QSlider(Qt::Horizontal, this);
    m_librarySizeSlider->setRange(120, 400);
    m_librarySizeSlider->setValue(GalleryView::kDefaultThumbnailSize);
    m_librarySizeSlider->setFixedWidth(140);
    searchBar->addWidget(m_libraryTagsEdit, 1);
    searchBar->addWidget(m_librarySearchButton);
    searchBar->addWidget(m_libraryLoadMoreButton);
    searchBar->addSpacing(16);
    searchBar->addWidget(new QLabel(tr("Rozmiar miniatur:"), this));
    searchBar->addWidget(m_librarySizeSlider);
    connect(m_librarySizeSlider, &QSlider::valueChanged, this, &LibraryTab::thumbnailSizeChanged);
    rootLayout->addLayout(searchBar);

    // Live nested-folder tree, hidden by default. Root set in refreshLibraryFolderTreeRoot().
    m_libraryFsModel = new QFileSystemModel(this);
    m_libraryFsModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

    m_libraryFolderTree = new QTreeView(this);
    m_libraryFolderTree->setModel(m_libraryFsModel);
    // Tree root has no visible row - see m_libraryRootFolderList for that.
    m_libraryFolderTree->setHeaderHidden(true);
    for (int col = 1; col < m_libraryFsModel->columnCount(); ++col)
        m_libraryFolderTree->hideColumn(col);
    m_libraryFolderTree->setSelectionMode(QAbstractItemView::SingleSelection);

    m_libraryRootFolderList = new QListWidget(this);
    m_libraryRootFolderList->addItem(tr("(folder główny)"));
    m_libraryRootFolderList->setCurrentRow(0);
    m_libraryRootFolderList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryRootFolderList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryRootFolderList->setFixedHeight(m_libraryRootFolderList->sizeHintForRow(0)
        + 2 * m_libraryRootFolderList->frameWidth() + 4);

    m_libraryTreePane = new QWidget(this);
    auto *treePaneLayout = new QVBoxLayout(m_libraryTreePane);
    treePaneLayout->setContentsMargins(0, 0, 0, 0);
    treePaneLayout->setSpacing(0);
    treePaneLayout->addWidget(m_libraryIncludeSubfoldersCheck);
    treePaneLayout->addWidget(m_libraryRootFolderList);
    treePaneLayout->addWidget(m_libraryFolderTree, 1);
    m_libraryTreePane->setMaximumWidth(280);
    m_libraryTreePane->hide();

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_libraryTreePane);
    m_libraryView = new QListView(splitter);
    m_libraryView->setModel(m_libraryModel);
    GalleryView::configure(m_libraryView);
    splitter->addWidget(m_libraryView);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);

    m_libraryStatusLabel = new QLabel(this);
    rootLayout->addWidget(m_libraryStatusLabel);
}

void LibraryTab::chooseDownloadDir()
{
    const QString oldDir = m_downloadManager->downloadDir();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder pobierania"), oldDir);
    if (dir.isEmpty())
        return;

    const QString oldCanonical = QDir(oldDir).canonicalPath();
    const QString newCanonical = QDir(dir).canonicalPath();
    const bool sameDir = oldCanonical == newCanonical;
    // One folder inside the other would copy the tree into itself.
    const bool nested = !sameDir && !oldCanonical.isEmpty() && !newCanonical.isEmpty()
        && (newCanonical.startsWith(oldCanonical + QLatin1Char('/'))
            || oldCanonical.startsWith(newCanonical + QLatin1Char('/')));

    QVector<Post> oldPosts;
    if (!sameDir && !nested && m_library && m_library->isOpen())
        oldPosts = m_library->search(QString(), -1, 0, oldDir, /*includeSubdirs=*/true);

    if (nested) {
        QMessageBox::information(this, tr("Folder pobierania"),
            tr("Jeden z folderów zawiera się w drugim, więc pliki nie zostaną przeniesione. "
               "Folder pobierania zostanie zmieniony, a dotychczasowe pliki zostaną na miejscu."));
    }

    bool moveLibrary = false;
    if (!oldPosts.isEmpty()) {
        moveLibrary = QMessageBox::question(this, tr("Folder pobierania"),
            tr("W starym folderze jest %1 plik(ów) z biblioteki. Przenieść je razem z podfolderami "
               "i tagami do nowego folderu?\n\nNie - pliki zostaną w starym miejscu i będą widoczne "
               "tylko w \"(wszystkie foldery)\".").arg(oldPosts.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;
    }

    m_downloadManager->setDownloadDir(dir);
    QSettings settings;
    settings.setValue(QStringLiteral("downloadDir"), dir);

    const QString relocateMessage = moveLibrary ? relocateLibrary(oldDir, dir, oldPosts) : QString();

    refreshLibraryFolderTreeRoot();
    populateLibraryFolders();
    searchLibrary();
    if (!relocateMessage.isEmpty())
        m_libraryStatusLabel->setText(relocateMessage);
}

QString LibraryTab::relocateLibrary(const QString &oldDir, const QString &newDir, const QVector<Post> &posts)
{
    const QDir oldRoot(oldDir);
    const QDir newRoot(newDir);

    // Recreates every subfolder, including empty ones.
    QDirIterator it(oldDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        newRoot.mkpath(oldRoot.relativeFilePath(it.filePath()));
    }

    QProgressDialog progress(tr("Przenoszenie biblioteki..."), tr("Anuluj"), 0, posts.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);

    // Path updates are written in batches: one transaction per file meant
    // one disk sync per file. Batches stay small so that a crash mid-move
    // leaves at most this many index entries behind the files on disk.
    constexpr int kPathBatchSize = 100;
    QVector<QPair<QString, QString>> pendingPaths;

    int moved = 0;
    int skipped = 0;
    for (int i = 0; i < posts.size(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled())
            break;

        const QString oldPath = posts.at(i).localFilePath;
        const QString newPath = newRoot.filePath(oldRoot.relativeFilePath(oldPath));
        QDir().mkpath(QFileInfo(newPath).absolutePath());
        if (QFile::exists(newPath) || !moveFile(oldPath, newPath)) {
            ++skipped;
            continue;
        }

        const QString oldSidecar = DownloadManager::sidecarPathFor(oldPath);
        if (QFile::exists(oldSidecar))
            moveFile(oldSidecar, DownloadManager::sidecarPathFor(newPath));
        pendingPaths.append({oldPath, newPath});
        if (pendingPaths.size() >= kPathBatchSize) {
            m_library->updatePostPaths(pendingPaths);
            pendingPaths.clear();
        }
        ++moved;
    }
    m_library->updatePostPaths(pendingPaths);
    progress.setValue(posts.size());

    return skipped == 0
        ? tr("Przeniesiono %1 plik(ów) do %2.").arg(moved).arg(newDir)
        : tr("Przeniesiono %1 plik(ów) do %2, %3 pominiętych (plik już istnieje, brak pliku lub błąd zapisu).")
              .arg(moved).arg(newDir).arg(skipped);
}

void LibraryTab::refreshLibraryFolderTreeRoot()
{
    // mkpath first: an empty/nonexistent dir falls back to the whole filesystem.
    const QString dir = m_downloadManager->downloadDir();
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);

    m_libraryFsModel->setRootPath(dir);
    m_libraryFolderTree->setRootIndex(m_libraryFsModel->index(dir));
}

void LibraryTab::searchLibrary()
{
    fetchLibraryPage(/*append=*/false);
}

void LibraryTab::loadMoreLibrary()
{
    fetchLibraryPage(/*append=*/true);
}

void LibraryTab::fetchLibraryPage(bool append)
{
    if (!m_library || !m_library->isOpen()) {
        m_libraryStatusLabel->setText(tr("Baza biblioteki niedostępna."));
        return;
    }

    if (!append) {
        m_libraryLastId = 0;
        m_libraryThumbnailLoader->cancelAll();
        m_libraryModel->clear();
    }
    const QString query = m_libraryTagsEdit->text().trimmed();

    // Folder filter runs in SQL, so it isn't limited to the newest page.
    QString dirFilter;
    const QString folderSelection = currentLibraryFolderSelection();
    if (folderSelection != QStringLiteral("*")) {
        const QString downloadDir = m_downloadManager->downloadDir();
        dirFilter = folderSelection.isEmpty() ? downloadDir : downloadDir + QLatin1Char('/') + folderSelection;
    }
    const bool includeSubfolders = m_libraryIncludeSubfoldersCheck && m_libraryIncludeSubfoldersCheck->isChecked();

    // Library posts come from several sources; cache each one's rules.
    const bool filtering = Blacklist::appliesToLibrary();
    QHash<QString, QStringList> rulesBySource;

    int hidden = 0;
    int fetched = 0;
    QVector<Post> visible;
    // A batch the blacklist empties completely would make "Wczytaj więcej" look
    // dead, so keep reading batches until something shows up or rows run out.
    for (int batch = 0; batch < kMaxAutoLoadChain; ++batch) {
        const QVector<Post> posts = m_library->search(query, kLibraryPageSize, m_libraryLastId,
                                                      dirFilter, includeSubfolders);
        fetched = posts.size();
        if (fetched == 0)
            break;
        // The cursor follows the raw rows, so filtered-out posts aren't re-read.
        m_libraryLastId = posts.last().libraryId;

        for (const Post &post : posts) {
            if (!filtering) {
                visible.append(post);
                continue;
            }
            if (!rulesBySource.contains(post.sourceName))
                rulesBySource.insert(post.sourceName, Blacklist::effectiveTags(post.sourceName));
            if (Blacklist::blocks(post, rulesBySource.value(post.sourceName)))
                ++hidden;
            else
                visible.append(post);
        }

        if (!visible.isEmpty() || fetched < kLibraryPageSize)
            break;
    }
    // A full last batch means the database still has rows below the cursor.
    m_libraryLoadMoreButton->setEnabled(fetched == kLibraryPageSize);

    m_libraryModel->appendPosts(visible);

    QString status = tr("Biblioteka: %1 wyników (z %2 pobranych łącznie)")
        .arg(m_libraryModel->rowCount()).arg(m_library->count());
    if (hidden > 0)
        status += QLatin1Char(' ') + tr("- ukryto %1 przez czarną listę").arg(hidden);
    m_libraryStatusLabel->setText(status);

    requestVisibleLibraryThumbnails();
}

void LibraryTab::populateLibraryFolders()
{
    const QString previousSelection = m_libraryFolderCombo->currentData().toString();
    const bool hadSelection = m_libraryFolderCombo->count() > 0;

    m_libraryFolderCombo->blockSignals(true);
    m_libraryFolderCombo->clear();
    m_libraryFolderCombo->addItem(tr("(wszystkie foldery)"), QStringLiteral("*"));
    m_libraryFolderCombo->addItem(tr("(folder główny)"), QString());
    for (const QString &folder : libraryFolderNames())
        m_libraryFolderCombo->addItem(folder, folder);

    if (hadSelection) {
        const int idx = m_libraryFolderCombo->findData(previousSelection);
        m_libraryFolderCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    m_libraryFolderCombo->blockSignals(false);

    // blockSignals() above skipped onLibraryFolderChanged(); update manually.
    updateLibraryDeleteFolderButtonState();
}

QStringList LibraryTab::libraryFolderNames() const
{
    // Recursive: returns every nested subfolder, not just direct children.
    const QString downloadDir = m_downloadManager->downloadDir();
    QDir root(downloadDir);
    QStringList names;
    QDirIterator it(downloadDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        names.append(root.relativeFilePath(it.filePath()));
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString LibraryTab::currentLibraryFolderSelection() const
{
    if (m_libraryTreeViewCheck->isChecked()) {
        const QModelIndex idx = m_libraryFolderTree->currentIndex();
        if (!idx.isValid())
            return QString(); // nothing selected in the tree = the root-folder row above it
        const QString downloadDirAbs = QDir(m_downloadManager->downloadDir()).absolutePath();
        const QString selectedAbs = QDir(m_libraryFsModel->filePath(idx)).absolutePath();
        if (selectedAbs == downloadDirAbs)
            return QString();
        return QDir(downloadDirAbs).relativeFilePath(selectedAbs);
    }
    return m_libraryFolderCombo ? m_libraryFolderCombo->currentData().toString() : QStringLiteral("*");
}

void LibraryTab::updateLibraryDeleteFolderButtonState()
{
    const QString selection = currentLibraryFolderSelection();
    m_deleteFolderButton->setEnabled(!selection.isEmpty() && selection != QStringLiteral("*"));
}

void LibraryTab::onLibraryFolderChanged(int index)
{
    Q_UNUSED(index);
    updateLibraryDeleteFolderButtonState();
    searchLibrary();
}

void LibraryTab::onLibraryFolderTreeSelectionChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    // Root-folder row and tree selection stay mutually exclusive.
    m_libraryRootFolderList->blockSignals(true);
    m_libraryRootFolderList->setCurrentRow(current.isValid() ? -1 : 0);
    m_libraryRootFolderList->blockSignals(false);

    updateLibraryDeleteFolderButtonState();
    searchLibrary();
}

void LibraryTab::toggleLibraryTreeView(bool enabled)
{
    m_libraryTreePane->setVisible(enabled);
    m_libraryFolderCombo->setDisabled(enabled);
    if (enabled) {
        // Re-sync in case the tree kept a stale selection while hidden.
        m_libraryRootFolderList->blockSignals(true);
        m_libraryRootFolderList->setCurrentRow(m_libraryFolderTree->currentIndex().isValid() ? -1 : 0);
        m_libraryRootFolderList->blockSignals(false);
    }
    updateLibraryDeleteFolderButtonState();
    searchLibrary();
}

void LibraryTab::createNewLibraryFolder()
{
    // In tree mode, creates a child of the currently selected folder.
    QString parentRelative;
    if (m_libraryTreeViewCheck->isChecked()) {
        const QString selection = currentLibraryFolderSelection();
        if (selection != QStringLiteral("*"))
            parentRelative = selection;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nowy folder"), tr("Nazwa folderu:"),
                                                QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, tr("Nowy folder"), tr("Nazwa folderu nie może zawierać \"/\" ani \"\\\"."));
        return;
    }

    const QString relativePath = parentRelative.isEmpty() ? name : parentRelative + QLatin1Char('/') + name;
    const QString fullTargetPath = m_downloadManager->downloadDir() + QLatin1Char('/') + relativePath;
    if (!FolderAccessGuard::isAllowed(fullTargetPath)) {
        QMessageBox::warning(this, tr("Nowy folder"), tr("Ta lokalizacja jest poza dozwolonym zakresem dostępu."));
        return;
    }
    QDir(m_downloadManager->downloadDir()).mkpath(relativePath);
    populateLibraryFolders();

    if (m_libraryTreeViewCheck->isChecked()) {
        const QModelIndex idx = m_libraryFsModel->index(fullTargetPath);
        if (idx.isValid()) {
            m_libraryFolderTree->setCurrentIndex(idx);
            m_libraryFolderTree->scrollTo(idx);
        }
    } else {
        const int idx = m_libraryFolderCombo->findData(relativePath);
        if (idx >= 0)
            m_libraryFolderCombo->setCurrentIndex(idx);
    }
}

void LibraryTab::deleteCurrentLibraryFolder()
{
    const QString folderName = currentLibraryFolderSelection();
    if (folderName.isEmpty() || folderName == QStringLiteral("*"))
        return; // guards against the "all"/root sentinel selections

    const QString downloadDir = m_downloadManager->downloadDir();
    const QString folderPath = downloadDir + QLatin1Char('/') + folderName;
    if (!FolderAccessGuard::isAllowed(folderPath)) {
        QMessageBox::warning(this, tr("Usuń folder"), tr("Ta lokalizacja jest poza dozwolonym zakresem dostępu."));
        return;
    }
    QDir dir(folderPath);
    if (!dir.exists())
        return;

    // Refuses to recurse; delete subfolders first.
    const QStringList subfolders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (!subfolders.isEmpty()) {
        QMessageBox::warning(this, tr("Usuń folder"),
            tr("Folder \"%1\" zawiera podfoldery (%2) - usuń je najpierw.")
                .arg(folderName, subfolders.join(QStringLiteral(", "))));
        return;
    }

    // Hidden tag sidecars don't count toward "this folder has files" - only
    // the real images do; sidecars just follow their image when it moves.
    const QStringList entries = dir.entryList(QDir::Files, QDir::Name);
    QStringList visibleFiles;
    for (const QString &entry : entries) {
        if (!entry.startsWith(QLatin1Char('.')))
            visibleFiles.append(entry);
    }

    const QString question = visibleFiles.isEmpty()
        ? tr("Usunąć pusty folder \"%1\"?").arg(folderName)
        : tr("Folder \"%1\" zawiera %2 plik(ów). Zostaną przeniesione do folderu głównego, "
             "a pusty folder zostanie usunięty. Kontynuować?").arg(folderName).arg(visibleFiles.size());

    if (QMessageBox::question(this, tr("Usuń folder"), question,
                               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    int skipped = 0;
    for (const QString &fileName : visibleFiles) {
        const QString oldPath = folderPath + QLatin1Char('/') + fileName;
        const QString newPath = downloadDir + QLatin1Char('/') + fileName;
        if (QFile::exists(newPath)) {
            ++skipped; // leave it behind rather than clobber/lose it silently
            continue;
        }
        // moveFile(): a subfolder can be a different mount than the root.
        if (!moveFile(oldPath, newPath)) {
            ++skipped; // counted, so the message below does not overstate
            continue;
        }

        const QString oldSidecar = DownloadManager::sidecarPathFor(oldPath);
        if (QFile::exists(oldSidecar))
            moveFile(oldSidecar, DownloadManager::sidecarPathFor(newPath));
        if (m_library)
            m_library->updatePostPath(oldPath, newPath);
    }

    // Only succeeds if truly empty; a moved-file collision blocks removal.
    const bool removed = dir.rmdir(folderPath);

    if (removed && m_libraryTreeViewCheck->isChecked()) {
        // Selection moves to the parent, or the root row if that's it.
        const QString parentPath = QFileInfo(folderPath).absolutePath();
        if (QDir(parentPath) == QDir(downloadDir)) {
            m_libraryFolderTree->setCurrentIndex(QModelIndex());
            m_libraryRootFolderList->setCurrentRow(0);
        } else {
            const QModelIndex parentIdx = m_libraryFsModel->index(parentPath);
            if (parentIdx.isValid())
                m_libraryFolderTree->setCurrentIndex(parentIdx);
        }
    }

    populateLibraryFolders();
    searchLibrary();

    if (!removed && skipped > 0) {
        m_libraryStatusLabel->setText(tr("Nie udało się usunąć folderu \"%1\" - %2 plik(ów) nie dało się przenieść.")
            .arg(folderName).arg(skipped));
    } else if (!removed) {
        // Nothing was skipped, so what blocks rmdir is leftover hidden files
        // (a tag sidecar whose image is already gone).
        dir.refresh();
        const QStringList leftover = dir.entryList(QDir::Files | QDir::Hidden, QDir::Name);
        m_libraryStatusLabel->setText(tr("Nie udało się usunąć folderu \"%1\" - zostało w nim %2 ukrytych plików.")
            .arg(folderName).arg(leftover.size()));
    } else if (!visibleFiles.isEmpty()) {
        m_libraryStatusLabel->setText(tr("Usunięto folder \"%1\", przeniesiono %2 plik(ów) do folderu głównego.")
            .arg(folderName).arg(visibleFiles.size() - skipped));
    } else {
        m_libraryStatusLabel->setText(tr("Usunięto pusty folder \"%1\".").arg(folderName));
    }
}

void LibraryTab::moveSelectedLibraryItems(const QString &targetFolderName)
{
    const QModelIndexList selected = m_libraryView->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return;

    const QString downloadDir = m_downloadManager->downloadDir();
    const QString targetDir = targetFolderName.isEmpty() ? downloadDir : downloadDir + QLatin1Char('/') + targetFolderName;
    if (!FolderAccessGuard::isAllowed(targetDir)) {
        m_libraryStatusLabel->setText(tr("Ta lokalizacja jest poza dozwolonym zakresem dostępu."));
        return;
    }
    QDir().mkpath(targetDir);

    int moved = 0;
    int skipped = 0;
    for (const QModelIndex &idx : selected) {
        const Post &post = m_libraryModel->postAt(idx.row());
        if (post.localFilePath.isEmpty())
            continue;

        const QFileInfo fi(post.localFilePath);
        const QString newPath = targetDir + QLatin1Char('/') + fi.fileName();
        if (newPath == post.localFilePath)
            continue; // already there

        if (QFile::exists(newPath)) {
            ++skipped; // don't clobber an existing file with the same name
            continue;
        }
        // moveFile(): the target folder can be a different mount.
        if (!moveFile(post.localFilePath, newPath)) {
            ++skipped;
            continue;
        }

        const QString oldSidecar = DownloadManager::sidecarPathFor(post.localFilePath);
        if (QFile::exists(oldSidecar))
            moveFile(oldSidecar, DownloadManager::sidecarPathFor(newPath));

        if (m_library)
            m_library->updatePostPath(post.localFilePath, newPath);
        ++moved;
    }

    populateLibraryFolders();
    searchLibrary();

    m_libraryStatusLabel->setText(skipped > 0
        ? tr("Przeniesiono %1 plik(ów), %2 pominiętych (plik już istnieje w celu albo błąd zapisu).").arg(moved).arg(skipped)
        : tr("Przeniesiono %1 plik(ów).").arg(moved));
}

void LibraryTab::onLibraryThumbnailReady(int row, QPixmap pixmap)
{
    if (!pixmap.isNull())
        m_libraryModel->setThumbnail(row, pixmap);
}

void LibraryTab::requestVisibleLibraryThumbnails()
{
    GalleryView::requestVisibleRows(m_libraryView, m_libraryModel, m_libraryThumbnailLoader);
    m_libraryAnimations->refresh();
}

void LibraryTab::setAnimatedThumbnails(bool enabled)
{
    m_libraryAnimations->setEnabled(enabled);
}

void LibraryTab::onLibraryClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    emit postClicked(m_libraryModel->postAt(index.row()), m_libraryModel->thumbnailAt(index.row()));
}

void LibraryTab::showLibraryContextMenu(const QPoint &pos)
{
    const QModelIndex clicked = m_libraryView->indexAt(pos);
    if (!clicked.isValid())
        return;

    if (!m_libraryView->selectionModel()->isSelected(clicked))
        m_libraryView->selectionModel()->select(clicked, QItemSelectionModel::ClearAndSelect);

    const Post post = m_libraryModel->postAt(clicked.row());
    const int selectedCount = m_libraryView->selectionModel()->selectedIndexes().size();

    QMenu menu(this);
    if (!post.localFilePath.isEmpty()) {
        QAction *openFileAction = menu.addAction(tr("Otwórz plik"));
        const QString path = post.localFilePath;
        connect(openFileAction, &QAction::triggered, this, [path]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });

        QAction *showInManagerAction = menu.addAction(tr("Pokaż w menedżerze plików"));
        const QString folder = QFileInfo(path).absolutePath();
        connect(showInManagerAction, &QAction::triggered, this, [folder]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });

        menu.addSeparator();

        QAction *copyImageAction = menu.addAction(tr("Kopiuj obraz do schowka"));
        connect(copyImageAction, &QAction::triggered, this, [path]() {
            const QImage image(path);
            if (!image.isNull())
                QGuiApplication::clipboard()->setImage(image);
        });

        QAction *copyPathAction = menu.addAction(tr("Kopiuj ścieżkę pliku"));
        connect(copyPathAction, &QAction::triggered, this, [path]() {
            QGuiApplication::clipboard()->setText(path);
        });

        QMenu *moveMenu = menu.addMenu(tr("Przenieś do... (%1)").arg(selectedCount));
        QAction *rootAction = moveMenu->addAction(tr("Folder główny"));
        connect(rootAction, &QAction::triggered, this, [this]() { moveSelectedLibraryItems(QString()); });
        const QStringList folders = libraryFolderNames();
        if (!folders.isEmpty())
            moveMenu->addSeparator();
        for (const QString &folderName : folders) {
            QAction *folderAction = moveMenu->addAction(folderName);
            connect(folderAction, &QAction::triggered, this, [this, folderName]() {
                moveSelectedLibraryItems(folderName);
            });
        }
        moveMenu->addSeparator();
        QAction *newFolderMoveAction = moveMenu->addAction(tr("Nowy folder..."));
        connect(newFolderMoveAction, &QAction::triggered, this, [this]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Nowy folder"), tr("Nazwa folderu:"),
                                                        QLineEdit::Normal, QString(), &ok).trimmed();
            if (ok && !name.isEmpty())
                moveSelectedLibraryItems(name);
        });
    }
    if (!post.tags.isEmpty()) {
        QAction *copyTagsAction = menu.addAction(tr("Kopiuj tagi"));
        const QString tagsText = post.tags.join(QLatin1Char(' '));
        connect(copyTagsAction, &QAction::triggered, this, [tagsText]() {
            QGuiApplication::clipboard()->setText(tagsText);
        });
    }
    if (!post.postPageUrl.isEmpty()) {
        QAction *openPageAction = menu.addAction(tr("Otwórz stronę posta w przeglądarce"));
        const QString url = post.postPageUrl;
        connect(openPageAction, &QAction::triggered, this, [url]() {
            QDesktopServices::openUrl(QUrl(url));
        });
    }

    menu.addSeparator();
    QAction *deleteAction = menu.addAction(tr("Usuń z dysku i biblioteki (%1)").arg(selectedCount));
    connect(deleteAction, &QAction::triggered, this, &LibraryTab::deleteSelectedLibraryItems);

    menu.exec(m_libraryView->viewport()->mapToGlobal(pos));
}

void LibraryTab::deleteSelectedLibraryItems()
{
    const QModelIndexList selected = m_libraryView->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return;

    const auto button = QMessageBox::question(this, tr("Usuń z biblioteki"),
        tr("Trwale usunąć %1 plik(ów) z dysku i biblioteki? Tej operacji nie można cofnąć.").arg(selected.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (button != QMessageBox::Yes)
        return;

    QList<int> rows;
    int failed = 0;
    for (const QModelIndex &idx : selected) {
        const Post &post = m_libraryModel->postAt(idx.row());
        if (!post.localFilePath.isEmpty()) {
            // Only the actual disk write is allow-list-gated - a stale/orphaned
            // DB row (file already gone, or outside the allow-list) must still
            // be removable, since that's just our own index, not a filesystem write.
            if (FolderAccessGuard::isAllowed(post.localFilePath) && QFile::exists(post.localFilePath)) {
                // A file that stays on disk keeps its row: dropping only the
                // index entry would hide a file the user believes is gone.
                if (!QFile::remove(post.localFilePath)) {
                    ++failed;
                    continue;
                }
                QFile::remove(DownloadManager::sidecarPathFor(post.localFilePath));
            }
            if (m_library)
                m_library->removePost(post.localFilePath);
        }
        rows.append(idx.row());
    }

    m_libraryModel->removeRows(rows);
    // Pending thumbnails are keyed by row, and removeRows() just shifted the
    // rows: drop them and request again for the new positions.
    m_libraryThumbnailLoader->cancelAll();
    m_libraryModel->resetMissingThumbnails();
    requestVisibleLibraryThumbnails();
    QString status = tr("Usunięto %1 plik(ów) (%2 wszystkich w bibliotece).")
        .arg(rows.size()).arg(m_library ? m_library->count() : 0);
    if (failed > 0)
        status += QLatin1Char(' ') + tr("%1 nie dało się usunąć (brak uprawnień?).").arg(failed);
    m_libraryStatusLabel->setText(status);
    emit postsDeleted();
}

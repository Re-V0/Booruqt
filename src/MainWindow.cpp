// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "MainWindow.h"
#include "AddSourceDialog.h"
#include "NetworkLog.h"
#include "ImageViewerDialog.h"
#include "FolderAccessGuard.h"
#include "ThemeManager.h"
#include "Blacklist.h"
#include "Paging.h"
#include "AppInfo.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QListView>
#include <QSlider>
#include <QSignalBlocker>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QDockWidget>
#include <QTabWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QProcess>
#include <QCoreApplication>
#include <QFileDialog>
#include <QInputDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QGuiApplication>
#include <QClipboard>
#include <QImage>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QScrollBar>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QStatusBar>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QShortcut>
#include <QCloseEvent>
#include <QTreeView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIcon>
#include <QFileSystemModel>
#include <QSplitter>
#include <QDirIterator>
#include <QRegularExpression>
#include <QLocale>
#include <QProgressDialog>
#include <QCompleter>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <utility>

namespace {

// Generous enough for the slow HTML-scrape bridge, not just plain APIs.
constexpr int kSearchTimeoutMs = 45000;
constexpr int kAutoscrollIntervalMs = 40;
constexpr int kInfiniteScrollThresholdPx = 80;
constexpr int kLibraryPageSize = 300;
// How many pages infinite scroll may pull by itself when the previous ones
// added nothing visible, before it gives up and waits for the user.
constexpr int kMaxAutoLoadChain = 5;
constexpr int kDefaultThumbnailSize = 220;
// The delegate draws the thumbnail inside this margin of its cell.
constexpr int kCellMargin = 4;

// Thumbnails are decoded at exactly the size they are drawn at, so painting
// never has to rescale them.
QSize thumbnailSizeForCell(const QSize &cell)
{
    return cell - QSize(2 * kCellMargin, 2 * kCellMargin);
}

// Paints each cell as a fixed-size, letterboxed, centered thumbnail.
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void setCellSize(const QSize &size) { m_cellSize = size; }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return m_cellSize;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect cellRect = option.rect;
        if (option.state & QStyle::State_Selected)
            painter->fillRect(cellRect, option.palette.highlight());
        else
            painter->fillRect(cellRect, option.palette.base());

        const QPixmap pixmap = index.data(GalleryModel::ThumbnailRole).value<QPixmap>();
        if (!pixmap.isNull()) {
            const QRect target = cellRect.adjusted(kCellMargin, kCellMargin, -kCellMargin, -kCellMargin);
            const QPixmap scaled = pixmap.scaled(target.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QPoint topLeft(target.x() + (target.width() - scaled.width()) / 2,
                                  target.y() + (target.height() - scaled.height()) / 2);
            painter->drawPixmap(topLeft, scaled);
        } else {
            painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(cellRect, Qt::AlignCenter, MainWindow::tr("..."));
        }

        painter->restore();
    }

private:
    QSize m_cellSize{kDefaultThumbnailSize, kDefaultThumbnailSize};
};

void configureGalleryView(QListView *view)
{
    view->setViewMode(QListView::IconMode);
    view->setResizeMode(QListView::Adjust);
    view->setMovement(QListView::Static);
    view->setUniformItemSizes(true);
    view->setSpacing(4);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setContextMenuPolicy(Qt::CustomContextMenu);

    const QSize cellSize(kDefaultThumbnailSize, kDefaultThumbnailSize);
    auto *delegate = new ThumbnailDelegate(view);
    delegate->setCellSize(cellSize);
    view->setItemDelegate(delegate);
    view->setGridSize(cellSize);
}

// Requests thumbnails for the rows inside the viewport. Rows are laid out in
// model order, so their tops never decrease going down the model: a binary
// search finds the first visible row and the loop stops at the first row
// below the viewport. The cost is the visible rows, not the whole model,
// which matters with autoscroll running every 40 ms over thousands of rows.
void requestVisibleRows(QListView *view, GalleryModel *model, ThumbnailLoader *loader)
{
    const int rowCount = model->rowCount();
    const int viewportHeight = view->viewport()->height();

    int low = 0;
    int high = rowCount;
    while (low < high) {
        const int mid = low + (high - low) / 2;
        if (view->visualRect(model->index(mid)).bottom() < 0)
            low = mid + 1;
        else
            high = mid;
    }

    for (int row = low; row < rowCount; ++row) {
        const QModelIndex idx = model->index(row);
        if (view->visualRect(idx).top() > viewportHeight)
            break;
        if (idx.data(GalleryModel::ThumbnailRequestedRole).toBool())
            continue;
        model->markThumbnailRequested(row);
        loader->request(row, QUrl(model->postAt(row).previewUrl));
    }
}

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_library(std::make_unique<LibraryDatabase>()),
      m_model(new GalleryModel(this)),
      m_thumbnailLoader(new ThumbnailLoader(this)),
      m_downloadManager(new DownloadManager(m_library.get(), this)),
      m_network(new QNetworkAccessManager(this))
{
    buildUi();

    QSettings settings;
    const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        + QStringLiteral("/blairqt");
    m_downloadManager->setDownloadDir(settings.value(QStringLiteral("downloadDir"), defaultDir).toString());
    refreshLibraryFolderTreeRoot();
    m_resultsPerPage = settings.value(QStringLiteral("resultsPerPage"), m_resultsPerPage).toInt();

    // The loaders start at the default cell; a slider change resizes them.
    m_thumbnailLoader->setTargetSize(thumbnailSizeForCell(QSize(kDefaultThumbnailSize, kDefaultThumbnailSize)));
    m_libraryThumbnailLoader->setTargetSize(thumbnailSizeForCell(QSize(kDefaultThumbnailSize, kDefaultThumbnailSize)));
    // Read before applyThemeIconSize(): moving the slider saves its value.
    const int savedThumbnailSize = settings.value(QStringLiteral("thumbnailSize"), 0).toInt();
    applyThemeIconSize();
    if (savedThumbnailSize >= m_sizeSlider->minimum() && savedThumbnailSize <= m_sizeSlider->maximum())
        m_sizeSlider->setValue(savedThumbnailSize);

    connect(m_thumbnailLoader, &ThumbnailLoader::thumbnailReady, this, &MainWindow::onThumbnailReady);
    connect(m_searchButton, &QPushButton::clicked, this, &MainWindow::startNewSearch);
    connect(m_tagsEdit, &QLineEdit::returnPressed, this, &MainWindow::startNewSearch);
    connect(m_loadMoreButton, &QPushButton::clicked, this, &MainWindow::loadNextPage);
    connect(m_prevPageButton, &QPushButton::clicked, this, &MainWindow::onPrevPage);
    connect(m_nextPageButton, &QPushButton::clicked, this, &MainWindow::onNextPage);
    connect(m_pageSpin, &QSpinBox::editingFinished, this, &MainWindow::onPageSpinEditingFinished);
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceComboChanged);
    connect(m_multiSourceButton, &QPushButton::clicked, this, &MainWindow::chooseMultipleSources);
    connect(m_deleteSourceButton, &QPushButton::clicked, this, &MainWindow::deleteCurrentSource);
    connect(m_autoscrollCheck, &QCheckBox::toggled, this, &MainWindow::toggleAutoscroll);
    connect(m_infiniteScrollCheck, &QCheckBox::toggled, this, &MainWindow::updatePagingControlsEnabled);
    connect(m_downloadButton, &QPushButton::clicked, this, &MainWindow::downloadSelected);

    connect(m_galleryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &MainWindow::requestVisibleThumbnails);
    connect(m_galleryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &MainWindow::maybeAutoLoadNextPage);
    connect(m_galleryView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::updateDownloadButtonLabel);
    connect(m_galleryView, &QListView::customContextMenuRequested,
            this, &MainWindow::showGalleryContextMenu);
    connect(m_galleryView, &QListView::clicked, this, &MainWindow::onGalleryClicked);

    connect(m_libraryThumbnailLoader, &ThumbnailLoader::thumbnailReady, this, &MainWindow::onLibraryThumbnailReady);
    connect(m_libraryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &MainWindow::requestVisibleLibraryThumbnails);
    connect(m_libraryView, &QListView::customContextMenuRequested,
            this, &MainWindow::showLibraryContextMenu);
    connect(m_libraryView, &QListView::clicked, this, &MainWindow::onLibraryClicked);
    connect(m_librarySearchButton, &QPushButton::clicked, this, &MainWindow::searchLibrary);
    connect(m_libraryLoadMoreButton, &QPushButton::clicked, this, &MainWindow::loadMoreLibrary);
    connect(m_libraryTagsEdit, &QLineEdit::returnPressed, this, &MainWindow::searchLibrary);
    connect(m_libraryFolderCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onLibraryFolderChanged);
    connect(m_newFolderButton, &QPushButton::clicked, this, &MainWindow::createNewLibraryFolder);
    connect(m_deleteFolderButton, &QPushButton::clicked, this, &MainWindow::deleteCurrentLibraryFolder);
    connect(m_libraryTreeViewCheck, &QCheckBox::toggled, this, &MainWindow::toggleLibraryTreeView);
    connect(m_libraryIncludeSubfoldersCheck, &QCheckBox::toggled, this, &MainWindow::searchLibrary);
    connect(m_libraryFolderTree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onLibraryFolderTreeSelectionChanged);
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
    connect(deleteShortcut, &QShortcut::activated, this, &MainWindow::deleteSelectedLibraryItems);

    connect(m_detailsPanel, &DetailsPanel::zoomRequested, this, &MainWindow::onZoomRequested);
    connect(m_detailsPanel, &DetailsPanel::downloadRequested, this, &MainWindow::onDetailsDownloadRequested);

    connect(m_downloadManager, &DownloadManager::downloadFinished, this,
            [this](const QString &path, bool ok, bool skipped, const QString &error) {
                if (ok && skipped)
                    m_statusLabel->setText(tr("Już pobrano: %1").arg(path));
                else if (ok)
                    m_statusLabel->setText(tr("Zapisano: %1").arg(path));
                else
                    m_statusLabel->setText(tr("Błąd pobierania: %1").arg(error));
            });

    connect(&NetworkLog::instance(), &NetworkLog::entryAdded, m_logView, &QPlainTextEdit::appendPlainText);

    m_autoscrollTimer = new QTimer(this);
    connect(m_autoscrollTimer, &QTimer::timeout, this, &MainWindow::autoscrollTick);

    populateSourceCombo();
    if (settings.value(QStringLiteral("welcomeCompleted"), false).toBool()) {
        const QStringList saved = settings.value(QStringLiteral("startupSources")).toStringList();
        for (const auto &source : m_sourceManager.sources()) {
            if (saved.contains(source->name()))
                m_selectedMergeSources.append(source->name());
        }
    } else {
        // Deferred: the welcome dialog should sit on top of a visible window.
        QTimer::singleShot(0, this, [this]() {
            runStartupSourcesDialog(/*firstRun=*/true);
            updateSourceSelectionUi();
            if (!m_selectedMergeSources.isEmpty())
                startNewSearch();
        });
    }
    updateSourceSelectionUi();
    updateDownloadButtonLabel();
    if (!m_selectedMergeSources.isEmpty())
        startNewSearch();
    else
        m_statusLabel->setText(tr("Brak źródeł startowych - wybierz źródło powyżej lub w Ustawieniach."));

    populateLibraryFolders();
    searchLibrary();

    // Restores dock and window layout saved in closeEvent().
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    restoreGeometry(settings.value(QStringLiteral("windowGeometry")).toByteArray());
}

void MainWindow::saveWindowLayout()
{
    QSettings settings;
    settings.setValue(QStringLiteral("windowState"), saveState());
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("B.LAIR_QT"));
    resize(1300, 850);

    // KDE's global menu can swallow the menu bar entirely; force in-window.
    menuBar()->setNativeMenuBar(false);

    // Grouped by what they affect; a flat list of ten entries hid everything.
    auto *settingsMenu = menuBar()->addMenu(tr("Ustawienia"));

    auto *searchMenu = settingsMenu->addMenu(tr("Wyszukiwanie"));
    QAction *startupSourcesAction = searchMenu->addAction(tr("Źródła startowe..."));
    connect(startupSourcesAction, &QAction::triggered, this, &MainWindow::chooseStartupSources);
    QAction *restoreSourcesAction = searchMenu->addAction(tr("Przywróć domyślne źródła"));
    connect(restoreSourcesAction, &QAction::triggered, this, &MainWindow::restoreDefaultSources);
    QAction *resultsPerPageAction = searchMenu->addAction(tr("Wyników na stronę..."));
    connect(resultsPerPageAction, &QAction::triggered, this, &MainWindow::chooseResultsPerPage);
    QAction *blacklistAction = searchMenu->addAction(tr("Czarna lista tagów..."));
    connect(blacklistAction, &QAction::triggered, this, &MainWindow::manageBlacklist);

    auto *libraryMenu = settingsMenu->addMenu(tr("Biblioteka"));
    QAction *downloadDirAction = libraryMenu->addAction(tr("Folder pobierania..."));
    connect(downloadDirAction, &QAction::triggered, this, &MainWindow::chooseDownloadDir);
    QAction *allowedFoldersAction = libraryMenu->addAction(tr("Dostęp do folderów..."));
    connect(allowedFoldersAction, &QAction::triggered, this, &MainWindow::manageAllowedFolders);

    auto *appearanceMenu = settingsMenu->addMenu(tr("Wygląd"));
    QAction *pickCuratedThemeAction = appearanceMenu->addAction(tr("Wybierz gotowy motyw..."));
    connect(pickCuratedThemeAction, &QAction::triggered, this, &MainWindow::pickCuratedTheme);
    QAction *openThemeAction = appearanceMenu->addAction(tr("Otwórz plik motywu (JSON)..."));
    connect(openThemeAction, &QAction::triggered, this, &MainWindow::openThemeFile);
    QAction *reloadThemeAction = appearanceMenu->addAction(tr("Przeładuj motyw"));
    connect(reloadThemeAction, &QAction::triggered, this, &MainWindow::reloadTheme);

    buildLanguageMenu(settingsMenu);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildSearchTab(), tr("Wyszukiwanie"));
    tabs->addTab(buildLibraryTab(), tr("Biblioteka"));
    setCentralWidget(tabs);

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);

    // Debug log: every request plus status/snippet on empty results.
    auto *logDock = new QDockWidget(tr("Log sieciowy"), this);
    // Object name must stay fixed; saveState() keys off it, not the title.
    logDock->setObjectName(QStringLiteral("networkLogDock"));
    m_logView = new QPlainTextEdit(logDock);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(2000);
    logDock->setWidget(m_logView);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);

    m_detailsDock = new QDockWidget(tr("Szczegóły"), this);
    m_detailsDock->setObjectName(QStringLiteral("detailsDock"));
    m_detailsPanel = new DetailsPanel(m_detailsDock);
    m_detailsDock->setWidget(m_detailsPanel);
    // Toggles let the user reopen a dock a click no longer force-opens.
    settingsMenu->addSeparator();
    settingsMenu->addAction(logDock->toggleViewAction());
    settingsMenu->addAction(m_detailsDock->toggleViewAction());
    addDockWidget(Qt::RightDockWidgetArea, m_detailsDock);
    resizeDocks({m_detailsDock}, {300}, Qt::Horizontal);

    setupTagCompleters();
}

void MainWindow::setupTagCompleters()
{
    // setWidget() instead of QLineEdit::setCompleter(): only the tag being
    // typed gets replaced, not the whole line. Arrow keys work in the popup.
    m_tagSuggestModel = new QStringListModel(this);
    m_tagCompleter = new QCompleter(m_tagSuggestModel, this);
    m_tagCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_tagCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_tagCompleter->setWidget(m_tagsEdit);
    connect(m_tagCompleter, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &tag) { m_tagsEdit->acceptCompletion(tag); });

    m_tagSuggestTimer = new QTimer(this);
    m_tagSuggestTimer->setSingleShot(true);
    m_tagSuggestTimer->setInterval(120);
    connect(m_tagSuggestTimer, &QTimer::timeout, this, &MainWindow::fetchTagSuggestions);
    connect(m_tagsEdit, &QLineEdit::textEdited, this, &MainWindow::onTagsEdited);

    m_libraryTagSuggestModel = new QStringListModel(this);
    m_libraryTagCompleter = new QCompleter(m_libraryTagSuggestModel, this);
    m_libraryTagCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_libraryTagCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_libraryTagCompleter->setWidget(m_libraryTagsEdit);
    connect(m_libraryTagCompleter, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &tag) { m_libraryTagsEdit->acceptCompletion(tag); });
    connect(m_libraryTagsEdit, &QLineEdit::textEdited, this, &MainWindow::onLibraryTagsEdited);

    // Installed after QCompleter's own filter, so this one sees keys first.
    m_tagCompleter->popup()->installEventFilter(this);
    m_libraryTagCompleter->popup()->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // With no row highlighted the list has nothing to accept, so the
            // keypress belongs to the search rather than to the completer.
            if (watched == m_tagCompleter->popup() && !m_tagCompleter->popup()->currentIndex().isValid()) {
                m_tagCompleter->popup()->hide();
                m_tagsEdit->clearSuggestion();
                startNewSearch();
                return true;
            }
            if (watched == m_libraryTagCompleter->popup()
                && !m_libraryTagCompleter->popup()->currentIndex().isValid()) {
                m_libraryTagCompleter->popup()->hide();
                m_libraryTagsEdit->clearSuggestion();
                searchLibrary();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onTagsEdited()
{
    const QString token = m_tagsEdit->currentToken();
    if (token.size() < 2) {
        m_tagSuggestTimer->stop();
        m_tagCompleter->popup()->hide();
        return;
    }
    m_tagSuggestPrefix = token;

    // Already-downloaded tags answer instantly; the remote reply replaces them.
    if (m_library) {
        const QStringList local = m_library->suggestTags(token, 10);
        if (!local.isEmpty())
            showTagSuggestions(m_tagCompleter, local, token);
    }
    m_tagSuggestTimer->start();
}

void MainWindow::fetchTagSuggestions()
{
    if (m_tagSuggestPrefix.isEmpty())
        return;

    if (m_tagSuggestReply) {
        QNetworkReply *previous = m_tagSuggestReply;
        m_tagSuggestReply = nullptr;
        previous->abort();
    }

    // In merged mode, the first selected source that actually has an endpoint -
    // not blindly the first one, which may have none.
    m_tagSuggestSource.clear();
    QNetworkRequest req;
    for (const QString &name : std::as_const(m_selectedMergeSources)) {
        const auto source = m_sourceManager.byName(name);
        if (!source)
            continue;
        const QNetworkRequest candidate = source->buildTagSuggestRequest(m_tagSuggestPrefix);
        if (candidate.url().isValid() && !candidate.url().isEmpty()) {
            m_tagSuggestSource = source;
            req = candidate;
            break;
        }
    }

    // No endpoint anywhere: fall back to already-downloaded tags.
    if (!req.url().isValid() || req.url().isEmpty()) {
        showTagSuggestions(m_tagCompleter,
            m_library ? m_library->suggestTags(m_tagSuggestPrefix, 10) : QStringList(), m_tagSuggestPrefix);
        return;
    }

    QNetworkRequest timedReq = req;
    timedReq.setTransferTimeout(5000);
    m_tagSuggestReply = m_network->get(timedReq);
    connect(m_tagSuggestReply, &QNetworkReply::finished, this, &MainWindow::onTagSuggestReplyFinished);
}

void MainWindow::onTagSuggestReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply != m_tagSuggestReply)
        return; // superseded by a newer keystroke
    m_tagSuggestReply = nullptr;

    QStringList tags;
    if (reply->error() == QNetworkReply::NoError && m_tagSuggestSource)
        tags = m_tagSuggestSource->parseTagSuggestions(reply->readAll());
    if (tags.isEmpty() && m_library)
        tags = m_library->suggestTags(m_tagSuggestPrefix, 10);

    showTagSuggestions(m_tagCompleter, tags, m_tagSuggestPrefix);
}

void MainWindow::onLibraryTagsEdited()
{
    const QString token = m_libraryTagsEdit->currentToken();
    if (token.size() < 2 || !m_library) {
        m_libraryTagCompleter->popup()->hide();
        return;
    }
    showTagSuggestions(m_libraryTagCompleter, m_library->suggestTags(token, 10), token);
}

void MainWindow::showTagSuggestions(QCompleter *completer, const QStringList &tags, const QString &prefix)
{
    auto *edit = qobject_cast<TagLineEdit *>(completer->widget());
    if (tags.isEmpty()) {
        if (edit)
            edit->clearSuggestion();
        completer->popup()->hide();
        return;
    }

    // Inline ghost text for the best match, popup list for the rest.
    if (edit)
        edit->setSuggestion(tags.first());
    qobject_cast<QStringListModel *>(completer->model())->setStringList(tags);
    completer->setCompletionPrefix(prefix);
    completer->complete();
}

QString MainWindow::defaultLanguageCode()
{
    const QStringList supported = {QStringLiteral("pl"), QStringLiteral("en"), QStringLiteral("de"),
                                   QStringLiteral("da"), QStringLiteral("ru")};
    for (const QString &name : QLocale::system().uiLanguages()) {
        const QString code = name.left(2).toLower();
        if (supported.contains(code))
            return code;
    }
    return QStringLiteral("en");
}

void MainWindow::buildLanguageMenu(QMenu *settingsMenu)
{
    // Language switch restarts the process instead of live-retranslating.
    auto *languageMenu = settingsMenu->addMenu(tr("Język"));
    auto *group = new QActionGroup(languageMenu);
    group->setExclusive(true);

    const QSettings settings;
    const QString currentLang = settings.value(QStringLiteral("language"), MainWindow::defaultLanguageCode()).toString();

    const QList<QPair<QString, QString>> languages = {
        {QStringLiteral("pl"), QStringLiteral("Polski")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("de"), QStringLiteral("Deutsch")},
        {QStringLiteral("da"), QStringLiteral("Dansk")},
        {QStringLiteral("ru"), QStringLiteral("Русский")},
    };

    for (const auto &[code, label] : languages) {
        QAction *action = languageMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(code == currentLang);
        group->addAction(action);
        m_languageActions.insert(code, action);
        connect(action, &QAction::triggered, this, [this, code]() { changeLanguage(code); });
    }
}

void MainWindow::changeLanguage(const QString &langCode)
{
    QSettings settings;
    const QString currentLang =
        settings.value(QStringLiteral("language"), MainWindow::defaultLanguageCode()).toString();
    if (currentLang == langCode)
        return;

    // The switch relaunches the process, which is not something to do silently.
    if (QMessageBox::question(this, tr("Język"),
            tr("Zmiana języka wymaga ponownego uruchomienia aplikacji. Uruchomić ponownie teraz?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
        if (QAction *action = m_languageActions.value(currentLang))
            action->setChecked(true);
        return;
    }

    settings.setValue(QStringLiteral("language"), langCode);
    // quit() below never delivers a close event, so persist the layout here.
    saveWindowLayout();
    settings.sync();

    // Relaunch rather than retranslate in place - see buildLanguageMenu().
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

QWidget *MainWindow::buildSearchTab()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);

    auto *searchBar = new QHBoxLayout();
    m_sourceCombo = new QComboBox(central);
    m_multiSourceButton = new QPushButton(tr("Wybierz kilka źródeł..."), central);
    m_multiSourceButton->setToolTip(tr("Przeszukaj kilka booru naraz - wyniki lądują we wspólnej galerii"));
    m_deleteSourceButton = new QPushButton(tr("Usuń"), central);
    m_deleteSourceButton->setToolTip(tr("Usuń bieżące źródło z listy"));
    m_tagsEdit = new TagLineEdit(central);
    m_tagsEdit->setPlaceholderText(tr("tag1 tag2 tag3..."));
    m_searchButton = new QPushButton(tr("Szukaj"), central);
    searchBar->addWidget(m_sourceCombo);
    searchBar->addWidget(m_multiSourceButton);
    searchBar->addWidget(m_deleteSourceButton);
    searchBar->addWidget(m_tagsEdit, 1);
    searchBar->addWidget(m_searchButton);
    rootLayout->addLayout(searchBar);

    m_galleryView = new QListView(central);
    m_galleryView->setModel(m_model);
    configureGalleryView(m_galleryView);
    rootLayout->addWidget(m_galleryView, 1);

    // Row 1: thumbnail size + paging controls + infinite scroll toggle + download.
    auto *pagingBar = new QHBoxLayout();
    auto *sizeLabel = new QLabel(tr("Rozmiar miniatur:"), central);
    m_sizeSlider = new QSlider(Qt::Horizontal, central);
    m_sizeSlider->setRange(120, 400);
    m_sizeSlider->setValue(kDefaultThumbnailSize);
    m_sizeSlider->setFixedWidth(140);

    m_prevPageButton = new QPushButton(tr("◀ Poprz."), central);
    m_pageSpin = new QSpinBox(central);
    m_pageSpin->setRange(1, 100000);
    m_pageSpin->setValue(1);
    m_nextPageButton = new QPushButton(tr("Nast. ▶"), central);
    m_infiniteScrollCheck = new QCheckBox(tr("Infinite scroll"), central);
    m_loadMoreButton = new QPushButton(tr("Wczytaj więcej"), central);
    m_downloadButton = new QPushButton(tr("Pobierz zaznaczone"), central);

    pagingBar->addWidget(sizeLabel);
    pagingBar->addWidget(m_sizeSlider);
    pagingBar->addSpacing(16);
    pagingBar->addWidget(m_prevPageButton);
    pagingBar->addWidget(new QLabel(tr("Strona:"), central));
    pagingBar->addWidget(m_pageSpin);
    pagingBar->addWidget(m_nextPageButton);
    pagingBar->addSpacing(16);
    pagingBar->addWidget(m_infiniteScrollCheck);
    pagingBar->addStretch(1);
    pagingBar->addWidget(m_loadMoreButton);
    pagingBar->addWidget(m_downloadButton);
    rootLayout->addLayout(pagingBar);

    // Row 2: autoscroll.
    auto *autoscrollBar = new QHBoxLayout();
    m_autoscrollCheck = new QCheckBox(tr("Autoscroll"), central);
    auto *speedLabel = new QLabel(tr("Szybkość:"), central);
    m_autoscrollSpeedSlider = new QSlider(Qt::Horizontal, central);
    m_autoscrollSpeedSlider->setRange(1, 30);
    m_autoscrollSpeedSlider->setValue(6);
    m_autoscrollSpeedSlider->setFixedWidth(140);
    autoscrollBar->addWidget(m_autoscrollCheck);
    autoscrollBar->addWidget(speedLabel);
    autoscrollBar->addWidget(m_autoscrollSpeedSlider);
    autoscrollBar->addStretch(1);
    rootLayout->addLayout(autoscrollBar);

    connect(m_sizeSlider, &QSlider::valueChanged, this, &MainWindow::onIconSizeChanged);

    return central;
}

QWidget *MainWindow::buildLibraryTab()
{
    m_libraryModel = new GalleryModel(this);
    // Library thumbnails are local files: nothing to cache.
    m_libraryThumbnailLoader = new ThumbnailLoader(this, /*useDiskCache=*/false);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);

    auto *folderBar = new QHBoxLayout();
    folderBar->addWidget(new QLabel(tr("Folder:"), central));
    m_libraryFolderCombo = new QComboBox(central);
    m_libraryTreeViewCheck = new QCheckBox(tr("Widok drzewa"), central);
    // Lives inside the tree pane: it only ever applied to tree selections.
    m_libraryIncludeSubfoldersCheck = new QCheckBox(tr("z podfolderami"), central);
    m_newFolderButton = new QPushButton(tr("Nowy folder..."), central);
    m_deleteFolderButton = new QPushButton(tr("Usuń folder..."), central);
    folderBar->addWidget(m_libraryFolderCombo, 1);
    folderBar->addWidget(m_libraryTreeViewCheck);
    folderBar->addWidget(m_newFolderButton);
    folderBar->addWidget(m_deleteFolderButton);
    rootLayout->addLayout(folderBar);

    auto *searchBar = new QHBoxLayout();
    m_libraryTagsEdit = new TagLineEdit(central);
    m_libraryTagsEdit->setPlaceholderText(tr("filtruj po tagach (puste = wszystko)..."));
    m_librarySearchButton = new QPushButton(tr("Szukaj w bibliotece"), central);
    m_libraryLoadMoreButton = new QPushButton(tr("Wczytaj więcej"), central);
    m_libraryLoadMoreButton->setEnabled(false);
    m_librarySizeSlider = new QSlider(Qt::Horizontal, central);
    m_librarySizeSlider->setRange(120, 400);
    m_librarySizeSlider->setValue(kDefaultThumbnailSize);
    m_librarySizeSlider->setFixedWidth(140);
    searchBar->addWidget(m_libraryTagsEdit, 1);
    searchBar->addWidget(m_librarySearchButton);
    searchBar->addWidget(m_libraryLoadMoreButton);
    searchBar->addSpacing(16);
    searchBar->addWidget(new QLabel(tr("Rozmiar miniatur:"), central));
    searchBar->addWidget(m_librarySizeSlider);
    connect(m_librarySizeSlider, &QSlider::valueChanged, this, &MainWindow::onIconSizeChanged);
    rootLayout->addLayout(searchBar);

    // Live nested-folder tree, hidden by default. Root set in refreshLibraryFolderTreeRoot().
    m_libraryFsModel = new QFileSystemModel(this);
    m_libraryFsModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

    m_libraryFolderTree = new QTreeView(central);
    m_libraryFolderTree->setModel(m_libraryFsModel);
    // Tree root has no visible row - see m_libraryRootFolderList for that.
    m_libraryFolderTree->setHeaderHidden(true);
    for (int col = 1; col < m_libraryFsModel->columnCount(); ++col)
        m_libraryFolderTree->hideColumn(col);
    m_libraryFolderTree->setSelectionMode(QAbstractItemView::SingleSelection);

    m_libraryRootFolderList = new QListWidget(central);
    m_libraryRootFolderList->addItem(tr("(folder główny)"));
    m_libraryRootFolderList->setCurrentRow(0);
    m_libraryRootFolderList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryRootFolderList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryRootFolderList->setFixedHeight(m_libraryRootFolderList->sizeHintForRow(0)
        + 2 * m_libraryRootFolderList->frameWidth() + 4);

    m_libraryTreePane = new QWidget(central);
    auto *treePaneLayout = new QVBoxLayout(m_libraryTreePane);
    treePaneLayout->setContentsMargins(0, 0, 0, 0);
    treePaneLayout->setSpacing(0);
    treePaneLayout->addWidget(m_libraryIncludeSubfoldersCheck);
    treePaneLayout->addWidget(m_libraryRootFolderList);
    treePaneLayout->addWidget(m_libraryFolderTree, 1);
    m_libraryTreePane->setMaximumWidth(280);
    m_libraryTreePane->hide();

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    splitter->addWidget(m_libraryTreePane);
    m_libraryView = new QListView(splitter);
    m_libraryView->setModel(m_libraryModel);
    configureGalleryView(m_libraryView);
    splitter->addWidget(m_libraryView);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);

    m_libraryStatusLabel = new QLabel(central);
    rootLayout->addWidget(m_libraryStatusLabel);

    return central;
}

void MainWindow::populateSourceCombo(const QString &selectName)
{
    m_sourceCombo->blockSignals(true);
    m_sourceCombo->clear();
    for (const auto &source : m_sourceManager.sources())
        m_sourceCombo->addItem(source->name());
    m_sourceCombo->addItem(tr("+ Dodaj własne booru..."));
    m_sourceCombo->setItemData(m_sourceCombo->count() - 1, true, Qt::UserRole);

    if (!selectName.isEmpty()) {
        const int idx = m_sourceCombo->findText(selectName);
        if (idx >= 0)
            m_sourceCombo->setCurrentIndex(idx);
    }
    m_sourceCombo->blockSignals(false);
}

void MainWindow::updateSourceSelectionUi()
{
    if (m_selectedMergeSources.size() <= 1) {
        m_sourceCombo->setEnabled(true);
        m_multiSourceButton->setText(tr("Wybierz kilka źródeł..."));
        m_deleteSourceButton->setEnabled(!m_selectedMergeSources.isEmpty());
        if (!m_selectedMergeSources.isEmpty()) {
            m_sourceCombo->blockSignals(true);
            const int idx = m_sourceCombo->findText(m_selectedMergeSources.first());
            if (idx >= 0)
                m_sourceCombo->setCurrentIndex(idx);
            m_sourceCombo->blockSignals(false);
        }
    } else {
        // Merged mode: the combo can't show multiple sources at once.
        m_sourceCombo->setEnabled(false);
        m_multiSourceButton->setText(tr("%1 źródeł zaznaczonych (zmień...)").arg(m_selectedMergeSources.size()));
        m_deleteSourceButton->setEnabled(false);
    }
}

void MainWindow::onSourceComboChanged(int index)
{
    if (index < 0)
        return;

    const bool isAddCustom = m_sourceCombo->itemData(index, Qt::UserRole).toBool();
    if (isAddCustom) {
        AddSourceDialog dialog(this);
        if (dialog.exec() == QDialog::Accepted && !dialog.detectedFlavor().isEmpty()) {
            m_sourceManager.addCustomSource(dialog.sourceName(), dialog.detectedFlavor(),
                                             dialog.baseUrl(), dialog.apiKey(), dialog.userId(),
                                             dialog.cookie(), dialog.userAgent(), dialog.htmlBridgeUrl(),
                                             dialog.scriptPath());
            populateSourceCombo(dialog.sourceName());
            m_selectedMergeSources = {dialog.sourceName()};
            updateSourceSelectionUi();
            startNewSearch();
        } else {
            // Cancelled: fall back to the source active before this dialog.
            populateSourceCombo(m_selectedMergeSources.value(0));
        }
        return;
    }

    // Collapses back to single-source, overriding any merged selection.
    m_selectedMergeSources = {m_sourceCombo->currentText()};
    startNewSearch();
}

void MainWindow::chooseMultipleSources()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Wybierz źródła do przeszukania"));
    dialog.resize(320, 420);

    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    for (const auto &source : m_sourceManager.sources()) {
        auto *item = new QListWidgetItem(source->name(), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_selectedMergeSources.contains(source->name()) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList selected;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked)
            selected.append(list->item(i)->text());
    }
    if (selected.isEmpty())
        return; // keep the previous selection rather than searching nothing

    m_selectedMergeSources = selected;
    updateSourceSelectionUi();
    startNewSearch();
}

void MainWindow::chooseStartupSources()
{
    runStartupSourcesDialog(false);
}

void MainWindow::runStartupSourcesDialog(bool firstRun)
{
    QSettings settings;
    const QStringList current = settings.value(QStringLiteral("startupSources")).toStringList();

    QDialog dialog(this);
    dialog.setWindowTitle(firstRun ? tr("Witamy w B.LAIR_QT") : tr("Źródła startowe"));
    dialog.resize(360, 460);

    auto *layout = new QVBoxLayout(&dialog);
    auto *info = new QLabel(tr("Niektóre serwisy booru domyślnie pokazują treści dla dorosłych. "
                                "Wybierz źródła, które mają wczytać się zaraz po uruchomieniu - "
                                "listę możesz zostawić pustą, aby zacząć od pustej karty Wyszukiwanie."), &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *list = new QListWidget(&dialog);
    for (const auto &source : m_sourceManager.sources()) {
        auto *item = new QListWidgetItem(source->name(), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool checked = firstRun ? source->name().contains(QStringLiteral("safe"), Qt::CaseInsensitive)
                                       : current.contains(source->name());
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);

    auto *buttonBox = new QDialogButtonBox(firstRun ? QDialogButtonBox::Ok
                                                      : (QDialogButtonBox::Ok | QDialogButtonBox::Cancel), &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() != QDialog::Accepted && !firstRun)
        return;

    QStringList selected;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked)
            selected.append(list->item(i)->text());
    }

    settings.setValue(QStringLiteral("startupSources"), selected);
    settings.setValue(QStringLiteral("welcomeCompleted"), true);

    if (firstRun) {
        m_selectedMergeSources = selected;
        updateSourceSelectionUi();
    }
}

void MainWindow::deleteCurrentSource()
{
    if (m_selectedMergeSources.size() != 1)
        return;
    const QString name = m_selectedMergeSources.first();

    if (QMessageBox::question(this, tr("Usuń źródło"),
            tr("Na pewno usunąć \"%1\" z listy źródeł?").arg(name)) != QMessageBox::Yes) {
        return;
    }

    m_sourceManager.removeSource(name);
    populateSourceCombo();
    m_selectedMergeSources = m_sourceCombo->count() > 0 ? QStringList{m_sourceCombo->itemText(0)} : QStringList{};
    updateSourceSelectionUi();
    if (!m_selectedMergeSources.isEmpty())
        startNewSearch();
    else
        m_model->clear();
}

void MainWindow::restoreDefaultSources()
{
    m_sourceManager.restoreDefaultSources();
    populateSourceCombo();
    m_selectedMergeSources = m_sourceCombo->count() > 0 ? QStringList{m_sourceCombo->itemText(0)} : QStringList{};
    updateSourceSelectionUi();
    m_statusLabel->setText(tr("Przywrócono domyślną listę źródeł."));
}

void MainWindow::onIconSizeChanged(int value)
{
    const QSize size(value, value);

    const auto syncSlider = [value](QSlider *slider) {
        if (!slider || slider->value() == value)
            return;
        const QSignalBlocker blocker(slider);
        slider->setValue(value);
    };
    const auto resizeCells = [size](QListView *view) {
        view->setGridSize(size);
        // Safe: this is the exact delegate we installed; no Q_OBJECT to cast with.
        static_cast<ThumbnailDelegate *>(view->itemDelegate())->setCellSize(size);
        view->doItemsLayout();
    };

    syncSlider(m_sizeSlider);
    m_thumbnailLoader->setTargetSize(thumbnailSizeForCell(size));
    resizeCells(m_galleryView);
    requestVisibleThumbnails();

    // Both tabs show this setting, but the library tab is built after the
    // search one, so its widgets can still be missing on the first call.
    if (!m_libraryView)
        return;

    syncSlider(m_librarySizeSlider);
    m_libraryThumbnailLoader->setTargetSize(thumbnailSizeForCell(size));
    resizeCells(m_libraryView);
    requestVisibleLibraryThumbnails();

    // Remembered across restarts; the theme's size is only the first default.
    QSettings settings;
    settings.setValue(QStringLiteral("thumbnailSize"), value);
}

void MainWindow::startNewSearch()
{
    if (m_selectedMergeSources.isEmpty())
        return;

    // issueSearchRequest() drops the previous round's replies and thumbnails.
    m_currentTags = m_tagsEdit->text().trimmed();
    issueSearchRequest(1, /*append=*/false);
}

void MainWindow::abortPendingSearchReplies()
{
    // abort() emits finished() synchronously, and onSearchReplyFinished()
    // removes the reply from m_pendingReplies. Iterating the set while that
    // happens is undefined and skips replies, so: copy, clear, then abort.
    const QSet<QNetworkReply *> replies = m_pendingReplies;
    m_pendingReplies.clear();
    for (QNetworkReply *reply : replies)
        reply->abort();
}

void MainWindow::loadNextPage()
{
    if (m_selectedMergeSources.isEmpty() || m_loading)
        return;
    issueSearchRequest(m_currentPage + 1, /*append=*/true);
}

void MainWindow::onPrevPage()
{
    if (m_currentPage <= 1 || m_loading)
        return;
    issueSearchRequest(m_currentPage - 1, /*append=*/false);
}

void MainWindow::onNextPage()
{
    if (m_loading)
        return;
    issueSearchRequest(m_currentPage + 1, /*append=*/false);
}

void MainWindow::onPageSpinEditingFinished()
{
    const int page = m_pageSpin->value();
    if (page == m_currentPage || m_loading)
        return;
    issueSearchRequest(page, /*append=*/false);
}

void MainWindow::issueSearchRequest(int page, bool append)
{
    if (m_selectedMergeSources.isEmpty()) {
        m_statusLabel->setText(tr("Nie wybrano żadnego źródła."));
        return;
    }

    abortPendingSearchReplies();

    if (!append) {
        // Row indices are about to be reused; drop in-flight thumbnails.
        m_thumbnailLoader->cancelAll();
        m_model->clear();
    }

    m_currentPage = page;
    m_pageSpin->blockSignals(true);
    m_pageSpin->setValue(page);
    m_pageSpin->blockSignals(false);

    if (!append)
        m_autoLoadChain = 0; // a fresh search starts the safety counter over

    m_loading = true;
    m_roundResultCount = 0;
    m_blockedThisRound = 0;
    m_rawRoundCount = 0;
    m_pendingBackgroundJobs = 0;
    ++m_searchRound;
    const QString sourcesLabel = m_selectedMergeSources.size() == 1
        ? m_selectedMergeSources.first()
        : tr("%1 źródłach").arg(m_selectedMergeSources.size());
    m_statusLabel->setText(tr("Wyszukiwanie w %1 (strona %2)...").arg(sourcesLabel).arg(page));
    updatePagingControlsEnabled();

    for (const QString &sourceName : m_selectedMergeSources) {
        auto source = m_sourceManager.byName(sourceName);
        if (!source) {
            NetworkLog::instance().log(QStringLiteral("Unknown source skipped: %1").arg(sourceName));
            continue;
        }

        // Sites that honour "-tag" skip blacklisted posts before they cost a
        // page slot. Reading the rules is cheap (cached); asking the source
        // whether it honours them is not, so that stays with buildSearchRequest.
        const QStringList blocked = Blacklist::effectiveTags(sourceName);
        const QString baseTags = m_currentTags;

        if (!source->mayBlock()) {
            const QString tags = searchTagsWithExclusions(*source, baseTags, blocked);
            sendSearchRequest(source, source->buildSearchRequest(tags, page, m_resultsPerPage));
            continue;
        }

        // A plugin spawns python3 both to answer supportsTagExclusion() and to
        // build the URL; neither may run here, or the window freezes.
        ++m_pendingBackgroundJobs;
        const int round = m_searchRound;
        const int perPage = m_resultsPerPage;
        auto *watcher = new QFutureWatcher<QNetworkRequest>(this);
        connect(watcher, &QFutureWatcher<QNetworkRequest>::finished, this, [this, watcher, source, round]() {
            watcher->deleteLater();
            if (round != m_searchRound)
                return; // a newer search already replaced this round
            --m_pendingBackgroundJobs;
            sendSearchRequest(source, watcher->result());
            finishSearchRoundIfDone();
        });
        watcher->setFuture(QtConcurrent::run([source, baseTags, blocked, page, perPage]() {
            const QString tags = searchTagsWithExclusions(*source, baseTags, blocked);
            return source->buildSearchRequest(tags, page, perPage);
        }));
    }

    finishSearchRoundIfDone();
}

void MainWindow::sendSearchRequest(const QSharedPointer<ISource> &source, QNetworkRequest request)
{
    if (!request.url().isValid() || request.url().isEmpty()) {
        NetworkLog::instance().log(QStringLiteral("  -> [%1] no usable request was built").arg(source->name()));
        return;
    }

    request.setTransferTimeout(kSearchTimeoutMs);
    NetworkLog::instance().log(QStringLiteral("GET %1").arg(NetworkLog::redactedUrl(request.url())));

    QNetworkReply *reply = m_network->get(request);
    m_pendingReplies.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, source]() {
        onSearchReplyFinished(reply, source);
    });
}

void MainWindow::finishSearchRoundIfDone()
{
    if (!m_loading || m_pendingBackgroundJobs > 0 || !m_pendingReplies.isEmpty())
        return;

    m_loading = false;
    // Raw, not filtered: a fully blacklisted page still has more pages after it.
    m_lastPageResultCount = m_rawRoundCount;
    updatePagingControlsEnabled();

    const QString sourcesLabel = m_selectedMergeSources.size() == 1
        ? m_selectedMergeSources.first()
        : tr("%1 źródeł").arg(m_selectedMergeSources.size());
    QString status = tr("%1: %2 wyników w widoku (strona %3, +%4)")
        .arg(sourcesLabel).arg(m_model->rowCount()).arg(m_currentPage).arg(m_roundResultCount);
    if (m_blockedThisRound > 0)
        status += QLatin1Char(' ') + tr("- ukryto %1 przez czarną listę").arg(m_blockedThisRound);
    m_statusLabel->setText(status);

    requestVisibleThumbnails();

    if (shouldChainAutoLoad(m_infiniteScrollCheck->isChecked(), m_rawRoundCount, m_roundResultCount,
                            m_galleryView->verticalScrollBar()->maximum(), m_autoLoadChain,
                            kMaxAutoLoadChain)) {
        ++m_autoLoadChain;
        // Queued: loadNextPage() would otherwise re-enter the round it ends.
        QTimer::singleShot(0, this, &MainWindow::loadNextPage);
    } else if (m_autoLoadChain >= kMaxAutoLoadChain && m_roundResultCount == 0) {
        m_autoLoadChain = 0;
        m_statusLabel->setText(tr("Czarna lista ukryła %1 kolejnych stron - przewiń, aby szukać dalej.")
                                   .arg(kMaxAutoLoadChain));
    }
}

void MainWindow::onSearchReplyFinished(QNetworkReply *reply, QSharedPointer<ISource> source)
{
    reply->deleteLater();
    // Already aborted/cleared by a newer search round; ignore it.
    if (!m_pendingReplies.remove(reply))
        return;

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        const bool timedOut = (reply->error() == QNetworkReply::TimeoutError
            || reply->error() == QNetworkReply::OperationCanceledError);
        // Log stays English regardless of UI language; only the status bar isn't.
        const QString logReason = timedOut
            ? QStringLiteral("timed out after %1s").arg(kSearchTimeoutMs / 1000)
            : reply->errorString();
        NetworkLog::instance().log(QStringLiteral("  -> ERROR [%1] (HTTP %2): %3").arg(source->name()).arg(httpStatus).arg(logReason));
        if (m_selectedMergeSources.size() == 1) {
            const QString displayReason = timedOut
                ? tr("przekroczono limit czasu (%1s) - sprawdź URL/połączenie").arg(kSearchTimeoutMs / 1000)
                : reply->errorString();
            m_statusLabel->setText(tr("Błąd (%1): %2").arg(source->name(), displayReason));
        }
    } else {
        const QByteArray data = reply->readAll();
        if (!source->mayBlock()) {
            handleSearchPosts(source, source->parseSearchResponse(data), httpStatus, data);
        } else {
            // Parsing runs the plugin a second time, so it goes off-thread too.
            ++m_pendingBackgroundJobs;
            const int round = m_searchRound;
            auto *watcher = new QFutureWatcher<QVector<Post>>(this);
            connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
                    [this, watcher, source, round, httpStatus, data]() {
                watcher->deleteLater();
                if (round != m_searchRound)
                    return;
                --m_pendingBackgroundJobs;
                handleSearchPosts(source, watcher->result(), httpStatus, data);
                finishSearchRoundIfDone();
            });
            watcher->setFuture(QtConcurrent::run([source, data]() {
                return source->parseSearchResponse(data);
            }));
            return;
        }
    }

    finishSearchRoundIfDone();
}

void MainWindow::handleSearchPosts(const QSharedPointer<ISource> &source, const QVector<Post> &parsed,
                                   int httpStatus, const QByteArray &data)
{
    m_rawRoundCount += parsed.size();

    // Runs even when the site already excluded them: the only guarantee.
    const QStringList blacklisted = Blacklist::effectiveTags(source->name());
    QVector<Post> posts;
    posts.reserve(parsed.size());
    for (const Post &post : parsed) {
        if (Blacklist::blocks(post, blacklisted))
            ++m_blockedThisRound;
        else
            posts.append(post);
    }

    m_roundResultCount += posts.size();
    m_model->appendPosts(posts);

    NetworkLog::instance().log(QStringLiteral("  -> [%1] HTTP %2, %3 bytes, %4 posts parsed, %5 blacklisted")
        .arg(source->name()).arg(httpStatus).arg(data.size()).arg(posts.size()).arg(parsed.size() - posts.size()));
    // Keyed off the parse, not the blacklist: this snippet diagnoses a site
    // that answered with something unreadable.
    if (parsed.isEmpty()) {
        const QString snippet = QString::fromUtf8(data.left(300)).simplified();
        NetworkLog::instance().log(QStringLiteral("     [%1] response snippet: %2").arg(source->name(), snippet));
    }
}

void MainWindow::onThumbnailReady(int row, QPixmap pixmap)
{
    if (!pixmap.isNull())
        m_model->setThumbnail(row, pixmap);
}

void MainWindow::requestVisibleThumbnails()
{
    requestVisibleRows(m_galleryView, m_model, m_thumbnailLoader);
}

void MainWindow::maybeAutoLoadNextPage()
{
    if (!m_infiniteScrollCheck->isChecked() || m_loading)
        return;

    QScrollBar *bar = m_galleryView->verticalScrollBar();
    if (bar->maximum() > 0 && bar->value() >= bar->maximum() - kInfiniteScrollThresholdPx) {
        // An empty last page means we've reached the end; stop fetching.
        if (m_lastPageResultCount == 0)
            return;
        m_autoLoadChain = 0; // the user scrolled, so scrolling still drives us
        loadNextPage();
    }
}

void MainWindow::toggleAutoscroll(bool enabled)
{
    updatePagingControlsEnabled();
    if (enabled)
        m_autoscrollTimer->start(kAutoscrollIntervalMs);
    else
        m_autoscrollTimer->stop();
}

void MainWindow::autoscrollTick()
{
    QScrollBar *bar = m_galleryView->verticalScrollBar();

    // At the bottom (or nothing to scroll yet): fetch more, hands-free.
    if (bar->maximum() <= 0 || bar->value() >= bar->maximum()) {
        if (m_loading)
            return;
        if (m_lastPageResultCount == 0) {
            m_autoscrollCheck->setChecked(false);
            m_statusLabel->setText(tr("Koniec galerii - autoscroll zatrzymany."));
            return;
        }
        m_autoLoadChain = 0; // the timer keeps driving us; no self-load needed
        loadNextPage();
        return;
    }

    bar->setValue(bar->value() + m_autoscrollSpeedSlider->value());
}

void MainWindow::updatePagingControlsEnabled()
{
    // Infinite scroll and autoscroll keep appending pages to one view, so a
    // page number would describe nothing the user can see.
    const bool accumulating = m_infiniteScrollCheck->isChecked() || m_autoscrollCheck->isChecked();
    const bool idle = !m_loading;

    m_searchButton->setEnabled(idle);
    m_loadMoreButton->setEnabled(idle);
    m_pageSpin->setEnabled(idle && !accumulating);
    m_prevPageButton->setEnabled(idle && !accumulating && m_currentPage > 1);
    m_nextPageButton->setEnabled(idle && !accumulating);

    const QString hint = accumulating
        ? tr("Strony są nieaktywne, dopóki działa infinite scroll lub autoscroll.")
        : QString();
    m_pageSpin->setToolTip(hint);
    m_prevPageButton->setToolTip(hint);
    m_nextPageButton->setToolTip(hint);
}

void MainWindow::updateDownloadButtonLabel()
{
    const int count = m_galleryView->selectionModel()->selectedIndexes().size();
    m_downloadButton->setText(count > 0
        ? tr("Pobierz zaznaczone (%1)").arg(count)
        : tr("Pobierz zaznaczone"));
    m_downloadButton->setEnabled(count > 0);
}

void MainWindow::downloadSelected()
{
    const QModelIndexList selected = m_galleryView->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected)
        m_downloadManager->download(m_model->postAt(idx.row()));

    m_statusLabel->setText(tr("Pobieranie %1 plik(ów) do %2...")
        .arg(selected.size()).arg(m_downloadManager->downloadDir()));
}

void MainWindow::chooseDownloadDir()
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

QString MainWindow::relocateLibrary(const QString &oldDir, const QString &newDir, const QVector<Post> &posts)
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

void MainWindow::refreshLibraryFolderTreeRoot()
{
    // mkpath first: an empty/nonexistent dir falls back to the whole filesystem.
    const QString dir = m_downloadManager->downloadDir();
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);

    m_libraryFsModel->setRootPath(dir);
    m_libraryFolderTree->setRootIndex(m_libraryFsModel->index(dir));
}

void MainWindow::chooseResultsPerPage()
{
    bool ok = false;
    const int value = QInputDialog::getInt(this, tr("Wyników na stronę"),
        tr("Ile wyników pobierać na stronę (na źródło):"), m_resultsPerPage, 5, 200, 5, &ok);
    if (!ok || value == m_resultsPerPage)
        return;

    m_resultsPerPage = value;
    QSettings settings;
    settings.setValue(QStringLiteral("resultsPerPage"), value);

    // Otherwise the setting looks inert until the next manual search.
    if (!m_selectedMergeSources.isEmpty())
        startNewSearch();
}

void MainWindow::manageAllowedFolders()
{
    // Enforced by FolderAccessGuard, not this dialog.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Dostęp do folderów"));
    dialog.resize(420, 320);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Foldery, do których B.LAIR_QT ma dostęp:"), &dialog));

    auto *list = new QListWidget(&dialog);
    const QString appData = FolderAccessGuard::appDataFolder();
    const QString downloadDir = m_downloadManager->downloadDir();

    auto *appDataItem = new QListWidgetItem(tr("%1 (dane aplikacji)").arg(appData), list);
    appDataItem->setFlags(appDataItem->flags() & ~Qt::ItemIsEnabled);
    auto *downloadDirItem = new QListWidgetItem(tr("%1 (folder pobierania)").arg(downloadDir), list);
    downloadDirItem->setFlags(downloadDirItem->flags() & ~Qt::ItemIsEnabled);

    // Compared canonically: the guard stores canonical paths, while the
    // download dir may still be the symlinked form the user picked.
    const QString downloadDirCanonical = QFileInfo(downloadDir).canonicalFilePath();
    const QStringList extraFolders = FolderAccessGuard::allowedFolders();
    for (const QString &folder : extraFolders) {
        if (folder == downloadDir || QFileInfo(folder).canonicalFilePath() == downloadDirCanonical)
            continue; // already shown above as the (locked) download dir entry
        new QListWidgetItem(folder, list);
    }
    layout->addWidget(list, 1);

    auto *buttonBar = new QHBoxLayout();
    auto *addButton = new QPushButton(tr("Dodaj folder..."), &dialog);
    auto *removeButton = new QPushButton(tr("Usuń zaznaczony"), &dialog);
    removeButton->setEnabled(false);
    buttonBar->addWidget(addButton);
    buttonBar->addWidget(removeButton);
    buttonBar->addStretch(1);
    layout->addLayout(buttonBar);

    connect(list, &QListWidget::currentRowChanged, &dialog, [removeButton](int row) {
        removeButton->setEnabled(row >= 2); // rows 0/1 are the locked entries
    });
    // Edits stay in the list until OK; Cancel really cancels, like elsewhere.
    connect(addButton, &QPushButton::clicked, &dialog, [&dialog, list]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Wybierz folder"));
        if (dir.isEmpty())
            return;
        new QListWidgetItem(dir, list);
    });
    connect(removeButton, &QPushButton::clicked, &dialog, [list]() {
        delete list->currentItem();
    });

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList kept;
    for (int row = 2; row < list->count(); ++row) // rows 0/1 are the locked entries
        kept.append(list->item(row)->text());

    for (const QString &folder : FolderAccessGuard::allowedFolders()) {
        if (kept.contains(folder))
            continue;
        // Never revoke the download folder: downloading would start failing.
        if (folder == downloadDir || QFileInfo(folder).canonicalFilePath() == downloadDirCanonical)
            continue;
        FolderAccessGuard::removeAllowedFolder(folder);
    }
    for (const QString &folder : std::as_const(kept))
        FolderAccessGuard::addAllowedFolder(folder);
}

void MainWindow::manageBlacklist()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Czarna lista tagów"));
    dialog.resize(420, 470);

    auto *layout = new QVBoxLayout(&dialog);
    auto *info = new QLabel(tr("Jeden tag w linii. Posty z tymi tagami nie trafią do wyników. "
                               "Gdy serwis to obsługuje, tagi są wykluczane już w zapytaniu, a w "
                               "pozostałych wypadkach wyniki są filtrowane po pobraniu. Dopasowanie "
                               "jest dokładne, więc \"solo\" nie ukryje \"solo_focus\"."), &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);

    static const QRegularExpression separators(QStringLiteral("[\\s,]+"));
    const auto tagCount = [](const QString &text) {
        return int(text.split(separators, Qt::SkipEmptyParts).size());
    };
    // The count makes a non-empty list for another source visible from here.
    const auto labelFor = [](const QString &name, int count) {
        return count > 0 ? QStringLiteral("%1  (%2)").arg(name).arg(count) : name;
    };

    auto *scopeCombo = new QComboBox(&dialog);
    const QString globalName = tr("(wszystkie źródła)");
    scopeCombo->addItem(labelFor(globalName, int(Blacklist::globalTags().size())), QString());
    scopeCombo->setItemData(0, globalName, Qt::UserRole + 1);
    for (const auto &source : m_sourceManager.sources()) {
        const int count = int(Blacklist::tagsForSource(source->name()).size());
        scopeCombo->addItem(labelFor(source->name(), count), source->name());
        scopeCombo->setItemData(scopeCombo->count() - 1, source->name(), Qt::UserRole + 1);
    }
    layout->addWidget(scopeCombo);

    auto *edit = new QPlainTextEdit(&dialog);
    layout->addWidget(edit, 1);

    auto *libraryCheck = new QCheckBox(tr("Stosuj też w Bibliotece (ukrywa pobrane pliki)"), &dialog);
    libraryCheck->setChecked(Blacklist::appliesToLibrary());
    layout->addWidget(libraryCheck);

    // Edits to other scopes survive switching the combo until OK/Cancel.
    QHash<QString, QString> pending;
    QString scope;
    const auto storedText = [](const QString &name) {
        const QStringList tags = name.isEmpty() ? Blacklist::globalTags() : Blacklist::tagsForSource(name);
        return tags.join(QLatin1Char('\n'));
    };
    edit->setPlainText(storedText(scope));

    connect(scopeCombo, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
        pending.insert(scope, edit->toPlainText());
        // Refresh the count on the entry being left, so it reflects the edit.
        const int previous = scopeCombo->findData(scope);
        if (previous >= 0) {
            scopeCombo->setItemText(previous,
                labelFor(scopeCombo->itemData(previous, Qt::UserRole + 1).toString(),
                         tagCount(edit->toPlainText())));
        }
        scope = scopeCombo->itemData(index).toString();
        edit->setPlainText(pending.contains(scope) ? pending.value(scope) : storedText(scope));
    });

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() != QDialog::Accepted)
        return;

    Blacklist::setAppliesToLibrary(libraryCheck->isChecked());

    pending.insert(scope, edit->toPlainText());
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        const QStringList tags = it.value().split(separators, Qt::SkipEmptyParts);
        if (it.key().isEmpty())
            Blacklist::setGlobalTags(tags);
        else
            Blacklist::setTagsForSource(it.key(), tags);
    }

    if (!m_selectedMergeSources.isEmpty())
        startNewSearch();
    searchLibrary();
}

void MainWindow::reloadTheme()
{
    ThemeManager::applyTheme();
    applyThemeIconSize();
    m_statusLabel->setText(tr("Motyw przeładowany."));
}

void MainWindow::applyThemeIconSize()
{
    const int iconSize = ThemeManager::defaultIconSize();
    if (iconSize >= m_sizeSlider->minimum() && iconSize <= m_sizeSlider->maximum())
        m_sizeSlider->setValue(iconSize);
}

void MainWindow::openThemeFile()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(ThemeManager::configFilePath()));
}

void MainWindow::pickCuratedTheme()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Wybierz gotowy motyw"));
    dialog.resize(300, 420);

    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);

    // Sentinel: empty name, restores the real system palette.
    QPixmap defaultSwatch(16, 16);
    defaultSwatch.fill(Qt::transparent);
    auto *defaultItem = new QListWidgetItem(QIcon(defaultSwatch), tr("Motyw domyślny (systemowy)"), list);
    defaultItem->setData(Qt::UserRole, QString());

    for (const auto &theme : ThemeManager::curatedThemes()) {
        // Accent-color swatch so entries are recognizable at a glance.
        const QJsonObject colors = QJsonDocument::fromJson(theme.colorsJson.toUtf8()).object();
        QColor swatchColor(colors.value(QStringLiteral("highlight")).toString());
        if (!swatchColor.isValid())
            swatchColor = Qt::gray;
        QPixmap swatch(16, 16);
        swatch.fill(swatchColor);

        auto *item = new QListWidgetItem(QIcon(swatch), theme.name, list);
        item->setData(Qt::UserRole, theme.name);
    }
    layout->addWidget(list);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return;

    const QString name = list->currentItem()->data(Qt::UserRole).toString();
    if (name.isEmpty())
        ThemeManager::applyDefaultTheme();
    else
        ThemeManager::applyCuratedTheme(name);

    applyThemeIconSize();
    m_statusLabel->setText(tr("Zastosowano motyw: %1").arg(list->currentItem()->text()));
}

void MainWindow::showGalleryContextMenu(const QPoint &pos)
{
    const QModelIndex clicked = m_galleryView->indexAt(pos);
    if (!clicked.isValid())
        return;

    if (!m_galleryView->selectionModel()->isSelected(clicked))
        m_galleryView->selectionModel()->select(clicked, QItemSelectionModel::ClearAndSelect);

    const int count = m_galleryView->selectionModel()->selectedIndexes().size();

    QMenu menu(this);
    QAction *downloadAction = menu.addAction(tr("Pobierz zaznaczone (%1)").arg(count));
    connect(downloadAction, &QAction::triggered, this, &MainWindow::downloadSelected);

    // Copy: menu.exec() spins an event loop that may reset the model.
    const Post post = m_model->postAt(clicked.row());

    const QString imageUrl = fullImageUrl(post);
    if (!imageUrl.isEmpty() && !isVideoUrl(imageUrl)) {
        QAction *copyImageAction = menu.addAction(tr("Kopiuj obraz do schowka"));
        connect(copyImageAction, &QAction::triggered, this, [this, imageUrl]() {
            m_statusLabel->setText(tr("Kopiowanie obrazu do schowka..."));
            QNetworkRequest req{QUrl(imageUrl)};
            req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
            req.setTransferTimeout(30000);
            QNetworkReply *reply = m_network->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    m_statusLabel->setText(tr("Nie udało się skopiować obrazu: %1").arg(reply->errorString()));
                    return;
                }
                const QImage image = QImage::fromData(reply->readAll());
                if (image.isNull()) {
                    m_statusLabel->setText(tr("Nie udało się zdekodować obrazu."));
                    return;
                }
                QGuiApplication::clipboard()->setImage(image);
                m_statusLabel->setText(tr("Skopiowano obraz do schowka."));
            });
        });
    }

    if (!post.postPageUrl.isEmpty()) {
        QAction *openAction = menu.addAction(tr("Otwórz stronę posta w przeglądarce"));
        const QString url = post.postPageUrl;
        connect(openAction, &QAction::triggered, this, [url]() {
            QDesktopServices::openUrl(QUrl(url));
        });
    }

    // Blacklisting straight from the offending post, instead of Settings.
    if (!post.tags.isEmpty()) {
        QMenu *blacklistMenu = menu.addMenu(tr("Dodaj tag do czarnej listy"));
        for (const QString &tag : post.tags) {
            QAction *tagAction = blacklistMenu->addAction(tag);
            connect(tagAction, &QAction::triggered, this, [this, tag]() {
                Blacklist::setGlobalTags(Blacklist::globalTags() + QStringList{tag});
                m_statusLabel->setText(tr("Dodano \"%1\" do czarnej listy.").arg(tag));
                startNewSearch();
            });
        }
    }

    menu.exec(m_galleryView->viewport()->mapToGlobal(pos));
}

void MainWindow::onGalleryClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    showPostInDetailsPanel(m_model->postAt(index.row()), m_model->thumbnailAt(index.row()));
}

void MainWindow::showPostInDetailsPanel(const Post &post, const QPixmap &preview)
{
    m_detailsPanel->showPost(post, preview);
    // Raises if already open; never force-opens a closed dock.
    if (m_detailsDock->isVisible())
        m_detailsDock->raise();
}

void MainWindow::searchLibrary()
{
    fetchLibraryPage(/*append=*/false);
}

void MainWindow::loadMoreLibrary()
{
    fetchLibraryPage(/*append=*/true);
}

void MainWindow::fetchLibraryPage(bool append)
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

void MainWindow::populateLibraryFolders()
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

QStringList MainWindow::libraryFolderNames() const
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

QString MainWindow::currentLibraryFolderSelection() const
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

void MainWindow::updateLibraryDeleteFolderButtonState()
{
    const QString selection = currentLibraryFolderSelection();
    m_deleteFolderButton->setEnabled(!selection.isEmpty() && selection != QStringLiteral("*"));
}

void MainWindow::onLibraryFolderChanged(int index)
{
    Q_UNUSED(index);
    updateLibraryDeleteFolderButtonState();
    searchLibrary();
}

void MainWindow::onLibraryFolderTreeSelectionChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    // Root-folder row and tree selection stay mutually exclusive.
    m_libraryRootFolderList->blockSignals(true);
    m_libraryRootFolderList->setCurrentRow(current.isValid() ? -1 : 0);
    m_libraryRootFolderList->blockSignals(false);

    updateLibraryDeleteFolderButtonState();
    searchLibrary();
}

void MainWindow::toggleLibraryTreeView(bool enabled)
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

void MainWindow::createNewLibraryFolder()
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

void MainWindow::deleteCurrentLibraryFolder()
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

void MainWindow::moveSelectedLibraryItems(const QString &targetFolderName)
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

void MainWindow::onLibraryThumbnailReady(int row, QPixmap pixmap)
{
    if (!pixmap.isNull())
        m_libraryModel->setThumbnail(row, pixmap);
}

void MainWindow::requestVisibleLibraryThumbnails()
{
    requestVisibleRows(m_libraryView, m_libraryModel, m_libraryThumbnailLoader);
}

void MainWindow::onLibraryClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    showPostInDetailsPanel(m_libraryModel->postAt(index.row()), m_libraryModel->thumbnailAt(index.row()));
}

void MainWindow::showLibraryContextMenu(const QPoint &pos)
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
    connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteSelectedLibraryItems);

    menu.exec(m_libraryView->viewport()->mapToGlobal(pos));
}

void MainWindow::deleteSelectedLibraryItems()
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
    m_detailsPanel->clear();
}

void MainWindow::onZoomRequested(const QString &imageUrl, const QString &title)
{
    auto *viewer = new ImageViewerDialog(imageUrl, title, this);
    viewer->show();
}

void MainWindow::onDetailsDownloadRequested(const Post &post)
{
    m_downloadManager->download(post);
}

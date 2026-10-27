// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "MainWindow.h"
#include "AnimatedThumbnails.h"
#include "DetailsPanel.h"
#include "DownloadManager.h"
#include "FolderAccessGuard.h"
#include "ImageViewerDialog.h"
#include "LibraryTab.h"
#include "NetworkLog.h"
#include "SearchTab.h"
#include "ThemeManager.h"
#include "Blacklist.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_library(std::make_unique<LibraryDatabase>()),
      m_downloadManager(new DownloadManager(m_library.get(), this))
{
    buildUi();

    QSettings settings;
    const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        + QStringLiteral("/blairqt");
    m_downloadManager->setDownloadDir(settings.value(QStringLiteral("downloadDir"), defaultDir).toString());
    m_libraryTab->refreshLibraryFolderTreeRoot();
    m_searchTab->setAnimatedThumbnails(AnimatedThumbnails::enabledInSettings());
    m_libraryTab->setAnimatedThumbnails(AnimatedThumbnails::enabledInSettings());

    // Read before applyThemeIconSize(): moving the slider saves its value.
    const int savedThumbnailSize = settings.value(QStringLiteral("thumbnailSize"), 0).toInt();
    applyThemeIconSize();
    m_searchTab->setThumbnailSliderValue(savedThumbnailSize);

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

    m_searchTab->start();
    m_libraryTab->start();

    // Restores dock and window layout saved in closeEvent().
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    restoreGeometry(settings.value(QStringLiteral("windowGeometry")).toByteArray());
}

void MainWindow::onThumbnailSizeChanged(int value)
{
    m_searchTab->applyThumbnailSize(value);
    m_libraryTab->applyThumbnailSize(value);

    // Remembered across restarts; the theme's size is only the first default.
    QSettings settings;
    settings.setValue(QStringLiteral("thumbnailSize"), value);
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
    QAction *restoreSourcesAction = searchMenu->addAction(tr("Przywróć domyślne źródła"));
    QAction *resultsPerPageAction = searchMenu->addAction(tr("Wyników na stronę..."));
    QAction *blacklistAction = searchMenu->addAction(tr("Czarna lista tagów..."));
    connect(blacklistAction, &QAction::triggered, this, &MainWindow::manageBlacklist);

    auto *libraryMenu = settingsMenu->addMenu(tr("Biblioteka"));
    QAction *downloadDirAction = libraryMenu->addAction(tr("Folder pobierania..."));
    QAction *allowedFoldersAction = libraryMenu->addAction(tr("Dostęp do folderów..."));
    connect(allowedFoldersAction, &QAction::triggered, this, &MainWindow::manageAllowedFolders);

    auto *appearanceMenu = settingsMenu->addMenu(tr("Wygląd"));
    QAction *pickCuratedThemeAction = appearanceMenu->addAction(tr("Wybierz gotowy motyw..."));
    connect(pickCuratedThemeAction, &QAction::triggered, this, &MainWindow::pickCuratedTheme);
    QAction *openThemeAction = appearanceMenu->addAction(tr("Otwórz plik motywu (JSON)..."));
    connect(openThemeAction, &QAction::triggered, this, &MainWindow::openThemeFile);
    QAction *reloadThemeAction = appearanceMenu->addAction(tr("Przeładuj motyw"));
    connect(reloadThemeAction, &QAction::triggered, this, &MainWindow::reloadTheme);
    appearanceMenu->addSeparator();
    // Off: only the zoom viewer plays video. On: the grid plays too.
    QAction *animatedAction = appearanceMenu->addAction(tr("Animowane miniatury (wideo, GIF)"));
    animatedAction->setCheckable(true);
    animatedAction->setChecked(AnimatedThumbnails::enabledInSettings());
    connect(animatedAction, &QAction::toggled, this, [this](bool enabled) {
        AnimatedThumbnails::saveEnabledInSettings(enabled);
        m_searchTab->setAnimatedThumbnails(enabled);
        m_libraryTab->setAnimatedThumbnails(enabled);
    });

    buildLanguageMenu(settingsMenu);

    auto *tabs = new QTabWidget(this);
    m_searchTab = new SearchTab(m_sourceManager, m_library.get(), m_downloadManager, this);
    tabs->addTab(m_searchTab, tr("Wyszukiwanie"));
    m_libraryTab = new LibraryTab(m_library.get(), m_downloadManager, this);
    tabs->addTab(m_libraryTab, tr("Biblioteka"));
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

    // Menu entries that act on one tab.
    connect(startupSourcesAction, &QAction::triggered, m_searchTab, &SearchTab::chooseStartupSources);
    connect(restoreSourcesAction, &QAction::triggered, m_searchTab, &SearchTab::restoreDefaultSources);
    connect(resultsPerPageAction, &QAction::triggered, m_searchTab, &SearchTab::chooseResultsPerPage);
    connect(downloadDirAction, &QAction::triggered, m_libraryTab, &LibraryTab::chooseDownloadDir);

    connect(m_searchTab, &SearchTab::statusMessage, m_statusLabel, &QLabel::setText);
    connect(m_searchTab, &SearchTab::thumbnailSizeChanged, this, &MainWindow::onThumbnailSizeChanged);
    connect(m_libraryTab, &LibraryTab::thumbnailSizeChanged, this, &MainWindow::onThumbnailSizeChanged);
    connect(m_searchTab, &SearchTab::postClicked, this, &MainWindow::showPostInDetailsPanel);
    connect(m_libraryTab, &LibraryTab::postClicked, this, &MainWindow::showPostInDetailsPanel);
    connect(m_libraryTab, &LibraryTab::postsDeleted, m_detailsPanel, &DetailsPanel::clear);
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

    // startNewSearch() does nothing while no source is selected.
    m_searchTab->startNewSearch();
    m_libraryTab->searchLibrary();
}

void MainWindow::reloadTheme()
{
    ThemeManager::applyTheme();
    applyThemeIconSize();
    m_statusLabel->setText(tr("Motyw przeładowany."));
}

void MainWindow::applyThemeIconSize()
{
    m_searchTab->setThumbnailSliderValue(ThemeManager::defaultIconSize());
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

void MainWindow::showPostInDetailsPanel(const Post &post, const QPixmap &preview)
{
    m_detailsPanel->showPost(post, preview);
    // Raises if already open; never force-opens a closed dock.
    if (m_detailsDock->isVisible())
        m_detailsDock->raise();
}

void MainWindow::onZoomRequested(const QString &url, bool video, const QString &title)
{
    auto *viewer = new ImageViewerDialog(url, video, title, this);
    viewer->show();
}

void MainWindow::onDetailsDownloadRequested(const Post &post)
{
    m_downloadManager->download(post);
}

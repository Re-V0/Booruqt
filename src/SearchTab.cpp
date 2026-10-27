// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SearchTab.h"
#include "SiteAccess.h"
#include "AddSourceDialog.h"
#include "AppInfo.h"
#include "Blacklist.h"
#include "DownloadManager.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "LibraryDatabase.h"
#include "NetworkLog.h"
#include "Paging.h"
#include "SourceManager.h"
#include "TagLineEdit.h"
#include "ThumbnailLoader.h"
#include "AnimatedThumbnails.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStringListModel>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <utility>

namespace {

// Generous enough for the slow HTML-scrape bridge, not just plain APIs.
constexpr int kSearchTimeoutMs = 45000;
constexpr int kAutoscrollIntervalMs = 40;
constexpr int kInfiniteScrollThresholdPx = 80;

} // namespace

SearchTab::SearchTab(SourceManager &sourceManager, LibraryDatabase *library,
                     DownloadManager *downloadManager, QWidget *parent)
    : QWidget(parent),
      m_sourceManager(sourceManager),
      m_library(library),
      m_model(new GalleryModel(this)),
      m_thumbnailLoader(new ThumbnailLoader(this)),
      m_downloadManager(downloadManager),
      m_network(new QNetworkAccessManager(this))
{
    buildUi();
    setupTagCompleter();
    m_animations = new AnimatedThumbnails(m_galleryView, m_model, this);

    const QSettings settings;
    m_resultsPerPage = settings.value(QStringLiteral("resultsPerPage"), m_resultsPerPage).toInt();

    // The loader starts at the default cell; a slider change resizes it.
    m_thumbnailLoader->setTargetSize(GalleryView::thumbnailSizeForCell(
        QSize(GalleryView::kDefaultThumbnailSize, GalleryView::kDefaultThumbnailSize)));

    connect(m_thumbnailLoader, &ThumbnailLoader::thumbnailReady, this, &SearchTab::onThumbnailReady);
    connect(m_searchButton, &QPushButton::clicked, this, &SearchTab::startNewSearch);
    connect(m_tagsEdit, &QLineEdit::returnPressed, this, &SearchTab::startNewSearch);
    connect(m_loadMoreButton, &QPushButton::clicked, this, &SearchTab::loadNextPage);
    connect(m_prevPageButton, &QPushButton::clicked, this, &SearchTab::onPrevPage);
    connect(m_nextPageButton, &QPushButton::clicked, this, &SearchTab::onNextPage);
    connect(m_pageSpin, &QSpinBox::editingFinished, this, &SearchTab::onPageSpinEditingFinished);
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this, &SearchTab::onSourceComboChanged);
    connect(m_multiSourceButton, &QPushButton::clicked, this, &SearchTab::chooseMultipleSources);
    connect(m_deleteSourceButton, &QPushButton::clicked, this, &SearchTab::deleteCurrentSource);
    connect(m_autoscrollCheck, &QCheckBox::toggled, this, &SearchTab::toggleAutoscroll);
    connect(m_infiniteScrollCheck, &QCheckBox::toggled, this, &SearchTab::updatePagingControlsEnabled);
    connect(m_downloadButton, &QPushButton::clicked, this, &SearchTab::downloadSelected);

    connect(m_galleryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SearchTab::requestVisibleThumbnails);
    connect(m_galleryView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SearchTab::maybeAutoLoadNextPage);
    connect(m_galleryView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &SearchTab::updateDownloadButtonLabel);
    connect(m_galleryView, &QListView::customContextMenuRequested,
            this, &SearchTab::showGalleryContextMenu);
    connect(m_galleryView, &QListView::clicked, this, &SearchTab::onGalleryClicked);

    m_autoscrollTimer = new QTimer(this);
    connect(m_autoscrollTimer, &QTimer::timeout, this, &SearchTab::autoscrollTick);
}

void SearchTab::start()
{
    const QSettings settings;
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
        emit statusMessage(tr("Brak źródeł startowych - wybierz źródło powyżej lub w Ustawieniach."));
}

void SearchTab::setThumbnailSliderValue(int value)
{
    if (value >= m_sizeSlider->minimum() && value <= m_sizeSlider->maximum())
        m_sizeSlider->setValue(value);
}

void SearchTab::applyThumbnailSize(int value)
{
    const QSize size(value, value);
    if (m_sizeSlider->value() != value) {
        const QSignalBlocker blocker(m_sizeSlider);
        m_sizeSlider->setValue(value);
    }
    m_thumbnailLoader->setTargetSize(GalleryView::thumbnailSizeForCell(size));
    GalleryView::setCellSize(m_galleryView, size);
    requestVisibleThumbnails();
}

void SearchTab::setupTagCompleter()
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
    connect(m_tagSuggestTimer, &QTimer::timeout, this, &SearchTab::fetchTagSuggestions);
    connect(m_tagsEdit, &QLineEdit::textEdited, this, &SearchTab::onTagsEdited);

    // Installed after QCompleter's own filter, so this one sees keys first.
    m_tagCompleter->popup()->installEventFilter(this);
}

bool SearchTab::eventFilter(QObject *watched, QEvent *event)
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
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SearchTab::onTagsEdited()
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

void SearchTab::fetchTagSuggestions()
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
    SiteAccess::apply(timedReq);
    m_tagSuggestReply = m_network->get(timedReq);
    connect(m_tagSuggestReply, &QNetworkReply::finished, this, &SearchTab::onTagSuggestReplyFinished);
}

void SearchTab::onTagSuggestReplyFinished()
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

void SearchTab::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);

    auto *searchBar = new QHBoxLayout();
    m_sourceCombo = new QComboBox(this);
    m_multiSourceButton = new QPushButton(tr("Wybierz kilka źródeł..."), this);
    m_multiSourceButton->setToolTip(tr("Przeszukaj kilka booru naraz - wyniki lądują we wspólnej galerii"));
    m_deleteSourceButton = new QPushButton(tr("Usuń"), this);
    m_deleteSourceButton->setToolTip(tr("Usuń bieżące źródło z listy"));
    m_tagsEdit = new TagLineEdit(this);
    m_tagsEdit->setPlaceholderText(tr("tag1 tag2 tag3..."));
    m_searchButton = new QPushButton(tr("Szukaj"), this);
    searchBar->addWidget(m_sourceCombo);
    searchBar->addWidget(m_multiSourceButton);
    searchBar->addWidget(m_deleteSourceButton);
    searchBar->addWidget(m_tagsEdit, 1);
    searchBar->addWidget(m_searchButton);
    rootLayout->addLayout(searchBar);

    m_galleryView = new QListView(this);
    m_galleryView->setModel(m_model);
    GalleryView::configure(m_galleryView);
    rootLayout->addWidget(m_galleryView, 1);

    // Row 1: thumbnail size + paging controls + infinite scroll toggle + download.
    auto *pagingBar = new QHBoxLayout();
    auto *sizeLabel = new QLabel(tr("Rozmiar miniatur:"), this);
    m_sizeSlider = new QSlider(Qt::Horizontal, this);
    m_sizeSlider->setRange(120, 400);
    m_sizeSlider->setValue(GalleryView::kDefaultThumbnailSize);
    m_sizeSlider->setFixedWidth(140);

    m_prevPageButton = new QPushButton(tr("◀ Poprz."), this);
    m_pageSpin = new QSpinBox(this);
    m_pageSpin->setRange(1, 100000);
    m_pageSpin->setValue(1);
    m_nextPageButton = new QPushButton(tr("Nast. ▶"), this);
    m_infiniteScrollCheck = new QCheckBox(tr("Infinite scroll"), this);
    m_loadMoreButton = new QPushButton(tr("Wczytaj więcej"), this);
    m_downloadButton = new QPushButton(tr("Pobierz zaznaczone"), this);

    pagingBar->addWidget(sizeLabel);
    pagingBar->addWidget(m_sizeSlider);
    pagingBar->addSpacing(16);
    pagingBar->addWidget(m_prevPageButton);
    pagingBar->addWidget(new QLabel(tr("Strona:"), this));
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
    m_autoscrollCheck = new QCheckBox(tr("Autoscroll"), this);
    auto *speedLabel = new QLabel(tr("Szybkość:"), this);
    m_autoscrollSpeedSlider = new QSlider(Qt::Horizontal, this);
    m_autoscrollSpeedSlider->setRange(1, 30);
    m_autoscrollSpeedSlider->setValue(6);
    m_autoscrollSpeedSlider->setFixedWidth(140);
    autoscrollBar->addWidget(m_autoscrollCheck);
    autoscrollBar->addWidget(speedLabel);
    autoscrollBar->addWidget(m_autoscrollSpeedSlider);
    autoscrollBar->addStretch(1);
    rootLayout->addLayout(autoscrollBar);

    connect(m_sizeSlider, &QSlider::valueChanged, this, &SearchTab::thumbnailSizeChanged);
}

void SearchTab::populateSourceCombo(const QString &selectName)
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

void SearchTab::updateSourceSelectionUi()
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

void SearchTab::onSourceComboChanged(int index)
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

void SearchTab::chooseMultipleSources()
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

void SearchTab::chooseStartupSources()
{
    runStartupSourcesDialog(false);
}

void SearchTab::runStartupSourcesDialog(bool firstRun)
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

void SearchTab::deleteCurrentSource()
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

void SearchTab::restoreDefaultSources()
{
    m_sourceManager.restoreDefaultSources();
    populateSourceCombo();
    m_selectedMergeSources = m_sourceCombo->count() > 0 ? QStringList{m_sourceCombo->itemText(0)} : QStringList{};
    updateSourceSelectionUi();
    emit statusMessage(tr("Przywrócono domyślną listę źródeł."));
}

void SearchTab::startNewSearch()
{
    if (m_selectedMergeSources.isEmpty())
        return;

    // issueSearchRequest() drops the previous round's replies and thumbnails.
    m_currentTags = m_tagsEdit->text().trimmed();
    issueSearchRequest(1, /*append=*/false);
}

void SearchTab::abortPendingSearchReplies()
{
    // abort() emits finished() synchronously, and onSearchReplyFinished()
    // removes the reply from m_pendingReplies. Iterating the set while that
    // happens is undefined and skips replies, so: copy, clear, then abort.
    const QSet<QNetworkReply *> replies = m_pendingReplies;
    m_pendingReplies.clear();
    for (QNetworkReply *reply : replies)
        reply->abort();
}

void SearchTab::loadNextPage()
{
    if (m_selectedMergeSources.isEmpty() || m_loading)
        return;
    issueSearchRequest(m_currentPage + 1, /*append=*/true);
}

void SearchTab::onPrevPage()
{
    if (m_currentPage <= 1 || m_loading)
        return;
    issueSearchRequest(m_currentPage - 1, /*append=*/false);
}

void SearchTab::onNextPage()
{
    if (m_loading)
        return;
    issueSearchRequest(m_currentPage + 1, /*append=*/false);
}

void SearchTab::onPageSpinEditingFinished()
{
    const int page = m_pageSpin->value();
    if (page == m_currentPage || m_loading)
        return;
    issueSearchRequest(page, /*append=*/false);
}

void SearchTab::issueSearchRequest(int page, bool append)
{
    if (m_selectedMergeSources.isEmpty()) {
        emit statusMessage(tr("Nie wybrano żadnego źródła."));
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
    emit statusMessage(tr("Wyszukiwanie w %1 (strona %2)...").arg(sourcesLabel).arg(page));
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

void SearchTab::sendSearchRequest(const QSharedPointer<ISource> &source, QNetworkRequest request)
{
    if (!request.url().isValid() || request.url().isEmpty()) {
        NetworkLog::instance().log(QStringLiteral("  -> [%1] no usable request was built").arg(source->name()));
        return;
    }

    request.setTransferTimeout(kSearchTimeoutMs);
    SiteAccess::apply(request);
    NetworkLog::instance().log(QStringLiteral("GET %1").arg(NetworkLog::redactedUrl(request.url())));

    QNetworkReply *reply = m_network->get(request);
    m_pendingReplies.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, source]() {
        onSearchReplyFinished(reply, source);
    });
}

void SearchTab::finishSearchRoundIfDone()
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
    emit statusMessage(status);

    requestVisibleThumbnails();

    if (shouldChainAutoLoad(m_infiniteScrollCheck->isChecked(), m_rawRoundCount, m_roundResultCount,
                            m_galleryView->verticalScrollBar()->maximum(), m_autoLoadChain,
                            kMaxAutoLoadChain)) {
        ++m_autoLoadChain;
        // Queued: loadNextPage() would otherwise re-enter the round it ends.
        QTimer::singleShot(0, this, &SearchTab::loadNextPage);
    } else if (m_autoLoadChain >= kMaxAutoLoadChain && m_roundResultCount == 0) {
        m_autoLoadChain = 0;
        emit statusMessage(tr("Czarna lista ukryła %1 kolejnych stron - przewiń, aby szukać dalej.")
                                   .arg(kMaxAutoLoadChain));
    }
}

void SearchTab::onSearchReplyFinished(QNetworkReply *reply, QSharedPointer<ISource> source)
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
            emit statusMessage(tr("Błąd (%1): %2").arg(source->name(), displayReason));
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

void SearchTab::handleSearchPosts(const QSharedPointer<ISource> &source, const QVector<Post> &parsed,
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

void SearchTab::onThumbnailReady(int row, QPixmap pixmap)
{
    if (!pixmap.isNull())
        m_model->setThumbnail(row, pixmap);
}

void SearchTab::requestVisibleThumbnails()
{
    GalleryView::requestVisibleRows(m_galleryView, m_model, m_thumbnailLoader);
    m_animations->refresh();
}

void SearchTab::setAnimatedThumbnails(bool enabled)
{
    m_animations->setEnabled(enabled);
}

void SearchTab::maybeAutoLoadNextPage()
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

void SearchTab::toggleAutoscroll(bool enabled)
{
    updatePagingControlsEnabled();
    if (enabled)
        m_autoscrollTimer->start(kAutoscrollIntervalMs);
    else
        m_autoscrollTimer->stop();
}

void SearchTab::autoscrollTick()
{
    QScrollBar *bar = m_galleryView->verticalScrollBar();

    // At the bottom (or nothing to scroll yet): fetch more, hands-free.
    if (bar->maximum() <= 0 || bar->value() >= bar->maximum()) {
        if (m_loading)
            return;
        if (m_lastPageResultCount == 0) {
            m_autoscrollCheck->setChecked(false);
            emit statusMessage(tr("Koniec galerii - autoscroll zatrzymany."));
            return;
        }
        m_autoLoadChain = 0; // the timer keeps driving us; no self-load needed
        loadNextPage();
        return;
    }

    bar->setValue(bar->value() + m_autoscrollSpeedSlider->value());
}

void SearchTab::updatePagingControlsEnabled()
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

void SearchTab::updateDownloadButtonLabel()
{
    const int count = m_galleryView->selectionModel()->selectedIndexes().size();
    m_downloadButton->setText(count > 0
        ? tr("Pobierz zaznaczone (%1)").arg(count)
        : tr("Pobierz zaznaczone"));
    m_downloadButton->setEnabled(count > 0);
}

void SearchTab::downloadSelected()
{
    const QModelIndexList selected = m_galleryView->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected)
        m_downloadManager->download(m_model->postAt(idx.row()));

    emit statusMessage(tr("Pobieranie %1 plik(ów) do %2...")
        .arg(selected.size()).arg(m_downloadManager->downloadDir()));
}

void SearchTab::chooseResultsPerPage()
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

void SearchTab::showGalleryContextMenu(const QPoint &pos)
{
    const QModelIndex clicked = m_galleryView->indexAt(pos);
    if (!clicked.isValid())
        return;

    if (!m_galleryView->selectionModel()->isSelected(clicked))
        m_galleryView->selectionModel()->select(clicked, QItemSelectionModel::ClearAndSelect);

    const int count = m_galleryView->selectionModel()->selectedIndexes().size();

    QMenu menu(this);
    QAction *downloadAction = menu.addAction(tr("Pobierz zaznaczone (%1)").arg(count));
    connect(downloadAction, &QAction::triggered, this, &SearchTab::downloadSelected);

    // Copy: menu.exec() spins an event loop that may reset the model.
    const Post post = m_model->postAt(clicked.row());

    const QString imageUrl = fullImageUrl(post);
    if (!imageUrl.isEmpty() && !isVideoUrl(imageUrl)) {
        QAction *copyImageAction = menu.addAction(tr("Kopiuj obraz do schowka"));
        connect(copyImageAction, &QAction::triggered, this, [this, imageUrl]() {
            emit statusMessage(tr("Kopiowanie obrazu do schowka..."));
            QNetworkRequest req{QUrl(imageUrl)};
            req.setHeader(QNetworkRequest::UserAgentHeader, blairqtUserAgent());
            req.setTransferTimeout(30000);
            SiteAccess::apply(req);
            QNetworkReply *reply = m_network->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    emit statusMessage(tr("Nie udało się skopiować obrazu: %1").arg(reply->errorString()));
                    return;
                }
                const QImage image = QImage::fromData(reply->readAll());
                if (image.isNull()) {
                    emit statusMessage(tr("Nie udało się zdekodować obrazu."));
                    return;
                }
                QGuiApplication::clipboard()->setImage(image);
                emit statusMessage(tr("Skopiowano obraz do schowka."));
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
                emit statusMessage(tr("Dodano \"%1\" do czarnej listy.").arg(tag));
                startNewSearch();
            });
        }
    }

    menu.exec(m_galleryView->viewport()->mapToGlobal(pos));
}

void SearchTab::onGalleryClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    emit postClicked(m_model->postAt(index.row()), m_model->thumbnailAt(index.row()));
}

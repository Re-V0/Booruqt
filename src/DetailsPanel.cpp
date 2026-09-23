// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DetailsPanel.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QUrl>

DetailsPanel::DetailsPanel(QWidget *parent)
    : QWidget(parent)
{
    m_previewLabel = new QLabel(this);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumHeight(200);
    m_previewLabel->setScaledContents(false);

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);

    m_tagsLabel = new QLabel(this);
    m_tagsLabel->setWordWrap(true);
    m_tagsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_tagsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_zoomButton = new QPushButton(tr("Powiększ"), this);
    m_openOriginalButton = new QPushButton(tr("Otwórz oryginał"), this);
    m_openPageButton = new QPushButton(tr("Strona posta"), this);
    m_downloadButton = new QPushButton(tr("Pobierz"), this);

    auto *buttonRow1 = new QHBoxLayout();
    buttonRow1->addWidget(m_zoomButton);
    buttonRow1->addWidget(m_downloadButton);

    auto *buttonRow2 = new QHBoxLayout();
    buttonRow2->addWidget(m_openOriginalButton);
    buttonRow2->addWidget(m_openPageButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_previewLabel);
    layout->addLayout(buttonRow1);
    layout->addLayout(buttonRow2);
    layout->addWidget(m_infoLabel);
    layout->addWidget(new QLabel(tr("<b>Tagi:</b>"), this));
    layout->addWidget(m_tagsLabel, 1);

    connect(m_zoomButton, &QPushButton::clicked, this, [this]() {
        if (!m_hasPost)
            return;
        const QString url = viewerImageUrl(m_currentPost);
        if (!url.isEmpty())
            emit zoomRequested(url, QStringLiteral("%1 #%2").arg(m_currentPost.sourceName, m_currentPost.id));
    });
    connect(m_openOriginalButton, &QPushButton::clicked, this, [this]() {
        if (m_hasPost && !m_currentPost.fileUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(m_currentPost.fileUrl));
    });
    connect(m_openPageButton, &QPushButton::clicked, this, [this]() {
        if (m_hasPost && !m_currentPost.postPageUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(m_currentPost.postPageUrl));
    });
    connect(m_downloadButton, &QPushButton::clicked, this, [this]() {
        if (m_hasPost)
            emit downloadRequested(m_currentPost);
    });

    clear();
}

void DetailsPanel::showPost(const Post &post, const QPixmap &preview)
{
    m_currentPost = post;
    m_hasPost = true;

    if (!preview.isNull()) {
        m_previewLabel->setPixmap(preview.scaled(280, 280, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        m_previewLabel->setText(tr("(brak podglądu)"));
    }

    m_infoLabel->setText(tr("<b>%1</b> #%2<br>%3x%4 &middot; ocena: %5 &middot; rating: %6")
        .arg(post.sourceName, post.id)
        .arg(post.width).arg(post.height)
        .arg(post.score)
        .arg(post.rating.isEmpty() ? tr("?") : post.rating));

    m_tagsLabel->setText(post.tags.isEmpty() ? tr("(brak tagów)") : post.tags.join(QStringLiteral(", ")));

    m_openOriginalButton->setEnabled(!post.fileUrl.isEmpty());
    m_openPageButton->setEnabled(!post.postPageUrl.isEmpty());
    // Empty means every candidate is video, which the viewer cannot decode.
    m_zoomButton->setEnabled(!viewerImageUrl(post).isEmpty());
    // Downloading needs the full file; without it the manager only errors out.
    m_downloadButton->setEnabled(!post.fileUrl.isEmpty());
}

void DetailsPanel::clear()
{
    m_hasPost = false;
    m_previewLabel->setText(tr("Kliknij obraz w galerii, aby zobaczyć szczegóły."));
    m_infoLabel->clear();
    m_tagsLabel->clear();
    m_openOriginalButton->setEnabled(false);
    m_openPageButton->setEnabled(false);
    m_zoomButton->setEnabled(false);
    m_downloadButton->setEnabled(false);
}

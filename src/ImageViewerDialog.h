// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>
#include <QString>
#include <QPixmap>
#include <QByteArray>
#include <QBuffer>
#include <memory>

class QLabel;
class QScrollArea;
class QCheckBox;
class QNetworkAccessManager;
class QResizeEvent;
class QMovie;

// Non-modal zoom viewer for a single post. Multi-frame files (GIF, animated
// WebP) play instead of showing a frozen first frame.
class ImageViewerDialog : public QDialog {
    Q_OBJECT
public:
    ImageViewerDialog(const QString &imageUrl, const QString &title, QWidget *parent = nullptr);
    // Defined in the .cpp: unique_ptr needs QMovie complete to destroy it.
    ~ImageViewerDialog() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onFitToggled(bool fit);

private:
    void showData(const QByteArray &data);
    bool startAnimation();
    void resizeToContent();
    void updateDisplayedImage();

    QNetworkAccessManager *m_network;
    QScrollArea *m_scrollArea;
    QLabel *m_imageLabel;
    QCheckBox *m_fitCheck;

    QPixmap m_fullPixmap;
    // QMovie streams from this buffer, so it must die before the buffer does.
    // Hence an owning pointer declared last - not a child of this dialog,
    // whose children are destroyed only after these members are gone.
    QByteArray m_data;
    QBuffer m_buffer;
    QSize m_contentSize;
    std::unique_ptr<QMovie> m_movie;
};

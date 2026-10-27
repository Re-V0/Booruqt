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
class QNetworkReply;
class QResizeEvent;
class QMovie;
class QMediaPlayer;
class QVideoWidget;
class QTemporaryFile;

// Non-modal zoom viewer for a single post. Multi-frame files (GIF, animated
// WebP) play instead of showing a frozen first frame. A video plays at once,
// in a loop and without sound; there are no player controls.
class ImageViewerDialog : public QDialog {
    Q_OBJECT
public:
    // video: play url as a video rather than decode it as an image.
    ImageViewerDialog(const QString &url, bool video, const QString &title, QWidget *parent = nullptr);
    // Defined in the .cpp: unique_ptr needs QMovie complete to destroy it,
    // and the video player must stop before its file is deleted.
    ~ImageViewerDialog() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onFitToggled(bool fit);

private:
    void loadImage(const QString &url);
    void showData(const QByteArray &data);
    bool startAnimation();
    void resizeToContent();
    void updateDisplayedImage();

    void loadVideo(const QString &url);
    void playVideo(const QUrl &file);
    void showMessage(const QString &text);

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

    // A remote video is downloaded into this file first, so the request
    // carries the site's cookie and User-Agent (SiteAccess) like any other.
    QNetworkReply *m_videoReply = nullptr;
    std::unique_ptr<QTemporaryFile> m_videoFile;
    QMediaPlayer *m_player = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
};

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

class QListView;
class QNetworkAccessManager;
class QTimer;
class GalleryModel;
class ThumbnailAnimation;

// Makes the video and GIF posts inside a grid's viewport move, without
// sound. Off until setEnabled(true); the setting is saved per user, not per
// tab.
//
// A playing row downloads its media (the video the viewer would play, or the
// GIF original) and decodes it; its frames replace the row's thumbnail in
// the model. To keep that affordable:
// - only rows inside the viewport play, at most kMaxPlaying at once;
// - a file larger than kMaxDownloadBytes is not fetched;
// - nothing starts while the view keeps scrolling (autoscroll included):
//   refresh() takes effect once the view has been still for a moment.
// A row that leaves the viewport stops and keeps its last frame.
class AnimatedThumbnails : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxPlaying = 6;
    static constexpr qint64 kMaxDownloadBytes = 32 * 1024 * 1024;

    AnimatedThumbnails(QListView *view, GalleryModel *model, QObject *parent = nullptr);
    ~AnimatedThumbnails() override;

    static bool enabledInSettings();
    static void saveEnabledInSettings(bool enabled);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Call when the visible rows may have changed (scroll, new results).
    // Showing, hiding and resizing the view are noticed without it.
    void refresh();

    // How long the view must be still before refresh() takes effect;
    // 300 ms by default.
    void setSettleDelay(int milliseconds);

    // Rows playing right now; for tests.
    QList<int> playingRows() const { return m_playing.keys(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyVisibleRows();
    void startRow(int row);
    void stopRow(int row);
    void stopAll();
    void onRowsInserted(int first);

    QListView *m_view;
    GalleryModel *m_model;
    QNetworkAccessManager *m_network;
    QTimer *m_settleTimer;
    bool m_enabled = false;
    QHash<int, ThumbnailAnimation *> m_playing;
    // Rows whose media failed or was too large: not tried again until the
    // rows change.
    QSet<int> m_failed;
};

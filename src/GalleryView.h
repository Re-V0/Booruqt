// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QSize>
#include <QVector>

class QListView;
class GalleryModel;
class ThumbnailLoader;

// The thumbnail grid both tabs show: a QListView in icon mode whose cells
// are drawn by a delegate at a fixed, adjustable size.
namespace GalleryView {

constexpr int kDefaultThumbnailSize = 220;

// Thumbnails are decoded at exactly the size they are drawn at, so painting
// does not have to rescale them. After the cell size changed they no longer
// are, until reloadThumbnails() has fetched them again.
QSize thumbnailSizeForCell(const QSize &cell);

// Icon mode, extended selection, a custom context menu, and the delegate,
// at the default cell size.
void configure(QListView *view);

// Resizes the cells of a view set up by configure().
void setCellSize(QListView *view, const QSize &size);

// The rows inside the viewport, in order.
QVector<int> visibleRows(const QListView *view, const GalleryModel *model);

// Requests thumbnails for the rows inside the viewport.
void requestVisibleRows(QListView *view, GalleryModel *model, ThumbnailLoader *loader);

// Fetches every thumbnail again at the loader's current target size: the
// visible rows now, the others when they scroll into view. What is on screen
// stays there, scaled, until its replacement arrives.
void reloadThumbnails(QListView *view, GalleryModel *model, ThumbnailLoader *loader);

// How long the size slider must rest before reloadThumbnails() runs: a drag
// passes through dozens of sizes, and each reload decodes every visible row.
constexpr int kThumbnailReloadDelayMs = 250;

} // namespace GalleryView

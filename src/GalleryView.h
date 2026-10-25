// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QSize>

class QListView;
class GalleryModel;
class ThumbnailLoader;

// The thumbnail grid both tabs show: a QListView in icon mode whose cells
// are drawn by a delegate at a fixed, adjustable size.
namespace GalleryView {

constexpr int kDefaultThumbnailSize = 220;

// Thumbnails are decoded at exactly the size they are drawn at, so painting
// never has to rescale them.
QSize thumbnailSizeForCell(const QSize &cell);

// Icon mode, extended selection, a custom context menu, and the delegate,
// at the default cell size.
void configure(QListView *view);

// Resizes the cells of a view set up by configure().
void setCellSize(QListView *view, const QSize &size);

// Requests thumbnails for the rows inside the viewport.
void requestVisibleRows(QListView *view, GalleryModel *model, ThumbnailLoader *loader);

} // namespace GalleryView

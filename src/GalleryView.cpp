// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "GalleryView.h"
#include "GalleryModel.h"
#include "ThumbnailLoader.h"

#include <QCoreApplication>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QUrl>

namespace {

// The delegate draws the thumbnail inside this margin of its cell.
constexpr int kCellMargin = 4;

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
            painter->drawText(cellRect, Qt::AlignCenter, QCoreApplication::translate("GalleryView", "..."));
        }

        painter->restore();
    }

private:
    QSize m_cellSize{GalleryView::kDefaultThumbnailSize, GalleryView::kDefaultThumbnailSize};
};

} // namespace

namespace GalleryView {

QSize thumbnailSizeForCell(const QSize &cell)
{
    return cell - QSize(2 * kCellMargin, 2 * kCellMargin);
}

void configure(QListView *view)
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

void setCellSize(QListView *view, const QSize &size)
{
    view->setGridSize(size);
    // Safe: configure() installed exactly this delegate; it has no Q_OBJECT
    // to cast with.
    static_cast<ThumbnailDelegate *>(view->itemDelegate())->setCellSize(size);
    view->doItemsLayout();
}

// Rows are laid out in model order, so their tops never decrease going down
// the model: a binary search finds the first visible row and the loop stops
// at the first row below the viewport. The cost is the visible rows, not the
// whole model, which matters with autoscroll running every 40 ms over
// thousands of rows.
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

} // namespace GalleryView

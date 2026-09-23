// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "TagLineEdit.h"

#include <QPainter>
#include <QKeyEvent>

TagLineEdit::TagLineEdit(QWidget *parent) : QLineEdit(parent)
{
    // Any edit invalidates the old suggestion until a new one arrives.
    connect(this, &QLineEdit::textEdited, this, &TagLineEdit::clearSuggestion);
}

QString TagLineEdit::tokenAt(const QString &text, int cursorPos)
{
    if (cursorPos <= 0)
        return {};
    const int start = text.lastIndexOf(QLatin1Char(' '), cursorPos - 1) + 1;
    return text.mid(start, cursorPos - start);
}

QString TagLineEdit::currentToken() const
{
    return tokenAt(text(), cursorPosition());
}

void TagLineEdit::acceptCompletion(const QString &tag)
{
    const int cursor = cursorPosition();
    const int start = cursor - int(currentToken().size());
    const QString current = text();
    // Don't double the separator when the caret already sits before one.
    const bool spaceFollows = cursor < current.size() && current.at(cursor) == QLatin1Char(' ');
    const QString inserted = spaceFollows ? tag : tag + QLatin1Char(' ');

    clearSuggestion();
    // Replace through the selection so this emits textEdited, exactly like
    // accepting the inline suggestion does - otherwise picking from the popup
    // would silently stop the next round of suggestions.
    setSelection(start, cursor - start);
    insert(inserted);
}

void TagLineEdit::setSuggestion(const QString &tag)
{
    const QString current = text();
    const QString token = currentToken();

    const bool usable = !token.isEmpty() && tag.size() > token.size()
        && tag.startsWith(token, Qt::CaseInsensitive)
        && cursorPosition() == current.size() && !hasSelectedText();

    const QString suffix = usable ? tag.mid(token.size()) : QString();
    if (suffix == m_suffix)
        return;
    m_suffix = suffix;
    update();
}

void TagLineEdit::clearSuggestion()
{
    if (m_suffix.isEmpty())
        return;
    m_suffix.clear();
    update();
}

void TagLineEdit::paintEvent(QPaintEvent *event)
{
    QLineEdit::paintEvent(event);
    if (m_suffix.isEmpty())
        return;

    // Blended, not the Disabled role: a JSON theme sets every color group
    // at once, which would make the ghost look like normal text.
    const QColor text = palette().color(QPalette::Text);
    const QColor base = palette().color(QPalette::Base);
    const QColor ghost((text.red() + base.red()) / 2, (text.green() + base.green()) / 2,
                       (text.blue() + base.blue()) / 2);

    const QRect cursor = cursorRect();
    QPainter painter(this);
    painter.setPen(ghost);
    painter.drawText(QRect(cursor.right(), cursor.top(), width() - cursor.right(), cursor.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, m_suffix);
}

void TagLineEdit::keyPressEvent(QKeyEvent *event)
{
    const bool accepts = (event->key() == Qt::Key_Right || event->key() == Qt::Key_End)
        && !event->modifiers().testFlag(Qt::ShiftModifier);
    if (!m_suffix.isEmpty() && accepts && cursorPosition() == text().size() && !hasSelectedText()) {
        const QString suffix = m_suffix;
        m_suffix.clear();
        insert(suffix); // emits textEdited, so the next suggestion is fetched
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void TagLineEdit::focusOutEvent(QFocusEvent *event)
{
    // The completer popup steals focus without ending the edit.
    if (event->reason() != Qt::PopupFocusReason)
        clearSuggestion();
    QLineEdit::focusOutEvent(event);
}

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QLineEdit>
#include <QStringList>

class QCompleter;

// Tag input with a fish-style inline suggestion: the rest of the best match
// is drawn greyed out after the cursor and accepted with Right or End.
class TagLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit TagLineEdit(QWidget *parent = nullptr);

    // Completes the tag being typed; ignored if it doesn't extend it.
    void setSuggestion(const QString &tag);
    void clearSuggestion();

    // The tag being typed, i.e. the one the caret sits in - not simply the
    // last word, so editing mid-query completes the right tag.
    QString currentToken() const;
    static QString tokenAt(const QString &text, int cursorPos);

    // Replaces that tag with the chosen one and leaves a trailing space.
    void acceptCompletion(const QString &tag);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    QString m_suffix;
};

// Shows tags in the completer's popup and the first one as the inline
// suggestion of the completer's TagLineEdit. No tags hides both.
void showTagSuggestions(QCompleter *completer, const QStringList &tags, const QString &prefix);

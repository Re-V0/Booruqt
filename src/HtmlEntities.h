// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>

// Some booru APIs return HTML-escaped text ("baldur&#039;s_gate_3").
inline QString decodeHtmlEntities(const QString &text)
{
    if (!text.contains(QLatin1Char('&')))
        return text;

    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('&')) {
            out.append(text.at(i));
            continue;
        }

        const int end = text.indexOf(QLatin1Char(';'), i + 1);
        if (end < 0 || end - i > 10) {
            out.append(text.at(i));
            continue;
        }

        const QStringView entity = QStringView(text).mid(i + 1, end - i - 1);
        // A QString, not a QChar: code points above U+FFFF (emoji) need two
        // UTF-16 units, and QChar(code) would truncate them (or assert).
        QString decoded;
        if (entity.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const bool hex = entity.size() > 1 && (entity.at(1) == QLatin1Char('x') || entity.at(1) == QLatin1Char('X'));
            const uint code = hex ? entity.mid(2).toUInt(&ok, 16) : entity.mid(1).toUInt(&ok, 10);
            const bool surrogate = code >= 0xD800 && code <= 0xDFFF;
            if (ok && code > 0 && code <= 0x10FFFF && !surrogate) {
                const char32_t codePoint = code;
                decoded = QString::fromUcs4(&codePoint, 1);
            }
        } else if (entity == QLatin1String("amp")) {
            decoded = QStringLiteral("&");
        } else if (entity == QLatin1String("lt")) {
            decoded = QStringLiteral("<");
        } else if (entity == QLatin1String("gt")) {
            decoded = QStringLiteral(">");
        } else if (entity == QLatin1String("quot")) {
            decoded = QStringLiteral("\"");
        } else if (entity == QLatin1String("apos")) {
            decoded = QStringLiteral("'");
        } else if (entity == QLatin1String("nbsp")) {
            decoded = QStringLiteral(" ");
        }

        if (decoded.isEmpty()) {
            out.append(text.at(i));
            continue;
        }
        out.append(decoded);
        i = end;
    }
    return out;
}

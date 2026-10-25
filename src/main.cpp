// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <QApplication>
#include <QTranslator>
#include <QSettings>
#include "MainWindow.h"
#include "ThemeManager.h"
#include "AppInfo.h"

namespace {

bool loadTranslation(QTranslator &translator, const QString &langCode)
{
    const QString fileName = QStringLiteral("blairqt_%1.qm").arg(langCode);
    return translator.load(findAppFile(QStringLiteral("translations/") + fileName));
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("blairqt"));
    QApplication::setOrganizationName(QStringLiteral("blairqt"));

    ThemeManager::applyTheme();

    const QSettings settings;
    const QString langCode = settings.value(QStringLiteral("language"), MainWindow::defaultLanguageCode()).toString();

    QTranslator translator;
    if (langCode != QStringLiteral("pl")) {
        if (loadTranslation(translator, langCode))
            app.installTranslator(&translator);
    }

    MainWindow window;
    window.show();

    return app.exec();
}

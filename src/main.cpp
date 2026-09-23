// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <QApplication>
#include <QTranslator>
#include <QSettings>
#include <QDir>
#include "MainWindow.h"
#include "ThemeManager.h"

namespace {

// Tries the dev-build path next to the binary, then the installed path.
bool loadTranslation(QTranslator &translator, const QString &langCode)
{
    const QString fileName = QStringLiteral("blairqt_%1.qm").arg(langCode);

    const QString devPath = QCoreApplication::applicationDirPath() + QStringLiteral("/translations/") + fileName;
    if (translator.load(devPath))
        return true;

#ifdef BLAIRQT_TRANSLATIONS_DIR
    const QString installedPath = QDir(QStringLiteral(BLAIRQT_TRANSLATIONS_DIR)).filePath(fileName);
    if (translator.load(installedPath))
        return true;
#endif

    return false;
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

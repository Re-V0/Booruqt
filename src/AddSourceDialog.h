// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>
#include <QString>

#include "SourceDetector.h"

class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QDialogButtonBox;
class QCheckBox;

// Registers a custom booru; SourceDetector works out its engine.
class AddSourceDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddSourceDialog(QWidget *parent = nullptr);

    QString sourceName() const;
    QString detectedFlavor() const { return m_detectedFlavor; }
    QString baseUrl() const;
    QString apiKey() const;
    QString userId() const;
    QString cookie() const;
    QString userAgent() const;
    QString htmlBridgeUrl() const;
    QString scriptPath() const;

private slots:
    void startTest();
    void onProbing(const QString &flavor, int index, int total, const QString &tag);
    void onDetectionFinished(const SourceDetector::Result &result);
    void applyPreset(int index);
    void onHtmlBridgeToggled(bool checked);
    void browseForScript();

private:
    QString normalizedBaseUrl() const;
    QString hintForIndex(int index) const;
    // Clears a previous successful test on any relevant edit.
    void invalidateDetectedFlavor();
    // Locks the fields a probe reads while a test runs.
    void setInputsEnabled(bool enabled);

    QComboBox *m_presetCombo;
    QLabel *m_presetHintLabel;
    QLineEdit *m_nameEdit;
    QLineEdit *m_urlEdit;
    QComboBox *m_flavorCombo;
    QLineEdit *m_apiKeyEdit;
    QLineEdit *m_userIdEdit;
    QLineEdit *m_cookieEdit;
    QLineEdit *m_userAgentEdit;
    QCheckBox *m_htmlBridgeCheck;
    QLineEdit *m_scriptPathEdit;
    QPushButton *m_scriptPathBrowseButton;
    QPushButton *m_testButton;
    QLabel *m_resultLabel;
    QPlainTextEdit *m_logEdit;
    QDialogButtonBox *m_buttonBox;

    SourceDetector *m_detector;
    QString m_detectedFlavor;
};

// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QNetworkRequest>
#include <QSharedPointer>
#include "Post.h"

class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QDialogButtonBox;
class QCheckBox;
class QNetworkAccessManager;
class QNetworkReply;
class ISource;

// Registers a custom booru by auto-probing each known flavor against it.
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
    void tryNextFlavor();
    // flavor travels with the reply; re-reading m_tryIndex could be stale.
    void onProbeReplyFinished(QNetworkReply *reply, const QString &flavor);
    void applyPreset(int index);
    void onHtmlBridgeToggled(bool checked);
    void browseForScript();

private:
    // Split so a blocking source (a plugin) can build and parse off-thread.
    void sendProbeRequest(QNetworkRequest request, const QString &flavor);
    void handleProbeResult(const QString &flavor, int httpStatus, const QVector<Post> &posts,
                           const QString &snippet);
    QString normalizedBaseUrl() const;
    QString hintForIndex(int index) const;
    // Clears a previous successful test on any relevant edit.
    void invalidateDetectedFlavor();

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

    QNetworkAccessManager *m_probeNetwork;
    QSharedPointer<ISource> m_probeSource;
    QStringList m_flavorsToTry;
    int m_tryIndex = 0;
    // Indexes into probeTags(); retries with a non-empty tag if needed.
    int m_probePass = 0;
    QString m_detectedFlavor;
};

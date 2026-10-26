// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "AddSourceDialog.h"
#include "SourceManager.h"
#include "sources/ISource.h"
#include "Post.h"
#include "NetworkLog.h"
#include "AppInfo.h"

#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>

namespace {
// Address of the manually-started html_scrape_bridge.py.
const QString kDefaultHtmlBridgeUrl = QStringLiteral("http://127.0.0.1:8765");

// name is also the pre-filled "Nazwa" field; hint text lives in hintForIndex().
struct SourcePreset {
    QString name;
    QString url;
    QString flavor;
};

// Gelbooru/Rule34 need an account now, so they're presets, not built-ins.
const QVector<SourcePreset> &sourcePresets()
{
    static const QVector<SourcePreset> presets = {
        {QStringLiteral("Gelbooru"), QStringLiteral("https://gelbooru.com"), QStringLiteral("gelbooru")},
        {QStringLiteral("Rule34"), QStringLiteral("https://rule34.xxx"), QStringLiteral("gelbooru")},
    };
    return presets;
}
}

AddSourceDialog::AddSourceDialog(QWidget *parent)
    : QDialog(parent), m_detector(new SourceDetector(this))
{
    setWindowTitle(tr("Dodaj własne booru"));
    setMinimumWidth(420);

    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("(wpisz ręcznie)"));
    for (const SourcePreset &preset : sourcePresets())
        m_presetCombo->addItem(preset.name);

    m_presetHintLabel = new QLabel(this);
    m_presetHintLabel->setWordWrap(true);
    m_presetHintLabel->setStyleSheet(QStringLiteral("color: palette(link);"));
    m_presetHintLabel->hide();

    auto *form = new QFormLayout();
    form->addRow(tr("Szybki start:"), m_presetCombo);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("np. MojeBooru"));
    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("https://example-booru.com"));

    m_flavorCombo = new QComboBox(this);
    m_flavorCombo->addItem(tr("Wykryj automatycznie"));
    for (const QString &f : SourceManager::knownFlavors())
        m_flavorCombo->addItem(f);
    // Manual-only: "python" needs a script, so it's not auto-probed.
    m_flavorCombo->addItem(QStringLiteral("python"));

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText(tr("opcjonalnie (np. Gelbooru api_key)"));
    m_userIdEdit = new QLineEdit(this);
    m_userIdEdit->setPlaceholderText(tr("opcjonalnie (np. Gelbooru user_id)"));

    // Manual Cloudflare bypass: paste a browser-solved cf_clearance cookie.
    m_cookieEdit = new QLineEdit(this);
    m_cookieEdit->setPlaceholderText(tr("opcjonalnie - cf_clearance=... (do stron za Cloudflare)"));
    m_userAgentEdit = new QLineEdit(this);
    m_userAgentEdit->setPlaceholderText(tr("opcjonalnie - User-Agent Twojej przeglądarki (musi pasować do cookie)"));

    // For API-less sites: scrapes HTML via the local Python bridge instead.
    m_htmlBridgeCheck = new QCheckBox(
        tr("Ta strona nie ma API (dapi) - scrapuj HTML przez lokalny most"), this);
    const QString bridgePath = QDir::toNativeSeparators(findAppFile(QStringLiteral("tools/html_scrape_bridge.py")));
    m_htmlBridgeCheck->setToolTip(tr("Uruchom go najpierw: python3 %1").arg(bridgePath));
    connect(m_htmlBridgeCheck, &QCheckBox::toggled, this, &AddSourceDialog::onHtmlBridgeToggled);

    // Only meaningful for the "python" flavor; shown unconditionally.
    m_scriptPathEdit = new QLineEdit(this);
    m_scriptPathEdit->setReadOnly(true);
    m_scriptPathEdit->setPlaceholderText(tr("wymagane dla mechanizmu \"python\" - kliknij Wybierz..."));
    m_scriptPathBrowseButton = new QPushButton(tr("Wybierz..."), this);
    connect(m_scriptPathBrowseButton, &QPushButton::clicked, this, &AddSourceDialog::browseForScript);
    auto *scriptPathRow = new QHBoxLayout();
    scriptPathRow->addWidget(m_scriptPathEdit, 1);
    scriptPathRow->addWidget(m_scriptPathBrowseButton);
    auto *scriptPathHint = new QLabel(
        tr("Uwaga: skrypt pluginu jest uruchamiany z pełnymi uprawnieniami Twojego konta - "
           "dodawaj tylko skrypty, którym ufasz."), this);
    scriptPathHint->setWordWrap(true);
    scriptPathHint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

    form->addRow(tr("Nazwa:"), m_nameEdit);
    form->addRow(tr("Adres URL:"), m_urlEdit);
    form->addRow(tr("Mechanizm:"), m_flavorCombo);
    form->addRow(tr("API key:"), m_apiKeyEdit);
    form->addRow(tr("User ID:"), m_userIdEdit);
    form->addRow(tr("Cookie:"), m_cookieEdit);
    form->addRow(tr("User-Agent:"), m_userAgentEdit);
    form->addRow(QString(), m_htmlBridgeCheck);
    form->addRow(tr("Skrypt pluginu (.py):"), scriptPathRow);
    form->addRow(QString(), scriptPathHint);
    form->addRow(QString(), m_presetHintLabel);

    m_testButton = new QPushButton(tr("Testuj"), this);
    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);

    // Shows each tried flavor's HTTP status and a body snippet.
    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumBlockCount(200);
    m_logEdit->setFixedHeight(140);
    m_logEdit->setPlaceholderText(tr("Wyniki testu poszczególnych mechanizmów pojawią się tutaj..."));
    // Mirrors the main window's network log here too, so e.g. a Python
    // plugin's own traceback (PythonPluginSource logs to NetworkLog) is
    // visible right in this dialog instead of only in the background dock.
    connect(&NetworkLog::instance(), &NetworkLog::entryAdded, m_logEdit, &QPlainTextEdit::appendPlainText);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_testButton);
    layout->addWidget(m_resultLabel);
    layout->addWidget(m_logEdit);
    layout->addWidget(m_buttonBox);

    connect(m_testButton, &QPushButton::clicked, this, &AddSourceDialog::startTest);
    connect(m_detector, &SourceDetector::probing, this, &AddSourceDialog::onProbing);
    connect(m_detector, &SourceDetector::logLine, m_logEdit, &QPlainTextEdit::appendPlainText);
    connect(m_detector, &SourceDetector::finished, this, &AddSourceDialog::onDetectionFinished);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Everything the probe request is built from; a change after a passed
    // test would otherwise be saved untested. The name is not among them.
    connect(m_urlEdit, &QLineEdit::textEdited, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_apiKeyEdit, &QLineEdit::textEdited, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_userIdEdit, &QLineEdit::textEdited, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_cookieEdit, &QLineEdit::textEdited, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_userAgentEdit, &QLineEdit::textEdited, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_flavorCombo, &QComboBox::currentTextChanged, this, &AddSourceDialog::invalidateDetectedFlavor);
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, &AddSourceDialog::applyPreset);
}

void AddSourceDialog::setInputsEnabled(bool enabled)
{
    // Locked while a test runs: each probe reads the fields again, so an edit
    // mid-test would mix two configurations into one result.
    m_presetCombo->setEnabled(enabled);
    m_urlEdit->setEnabled(enabled);
    m_flavorCombo->setEnabled(enabled && !m_htmlBridgeCheck->isChecked());
    m_apiKeyEdit->setEnabled(enabled);
    m_userIdEdit->setEnabled(enabled);
    m_cookieEdit->setEnabled(enabled);
    m_userAgentEdit->setEnabled(enabled);
    m_htmlBridgeCheck->setEnabled(enabled);
    m_scriptPathBrowseButton->setEnabled(enabled);
    m_testButton->setEnabled(enabled);
}

void AddSourceDialog::invalidateDetectedFlavor()
{
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_detectedFlavor.clear();
}

void AddSourceDialog::applyPreset(int index)
{
    const QVector<SourcePreset> &presets = sourcePresets();
    // Combo index 0 is the manual-entry placeholder; preset i is at i+1.
    if (index <= 0 || index > presets.size()) {
        m_presetHintLabel->hide();
        return;
    }

    const SourcePreset &preset = presets.at(index - 1);
    m_nameEdit->setText(preset.name);
    m_urlEdit->setText(preset.url);
    const int flavorIndex = m_flavorCombo->findText(preset.flavor);
    if (flavorIndex >= 0)
        m_flavorCombo->setCurrentIndex(flavorIndex);

    m_presetHintLabel->setText(hintForIndex(index));
    m_presetHintLabel->show();

    invalidateDetectedFlavor();
    m_apiKeyEdit->setFocus();
}

QString AddSourceDialog::hintForIndex(int index) const
{
    switch (index) {
    case 1:
        return tr("Gelbooru wymaga darmowego konta: My Account -> Options -> API Access Credentials, "
                   "skopiuj tam api_key i user_id i wklej poniżej.");
    case 2:
        return tr("Rule34.xxx też wymaga darmowego konta z wygenerowanym api_key i user_id "
                   "(ustawienia konta na rule34.xxx) - wklej je poniżej.");
    default:
        return {};
    }
}

QString AddSourceDialog::normalizedBaseUrl() const
{
    QString url = m_urlEdit->text().trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}

QString AddSourceDialog::sourceName() const { return m_nameEdit->text().trimmed(); }
QString AddSourceDialog::baseUrl() const { return normalizedBaseUrl(); }
QString AddSourceDialog::apiKey() const { return m_apiKeyEdit->text().trimmed(); }
QString AddSourceDialog::userId() const { return m_userIdEdit->text().trimmed(); }
QString AddSourceDialog::cookie() const { return m_cookieEdit->text().trimmed(); }
QString AddSourceDialog::userAgent() const { return m_userAgentEdit->text().trimmed(); }
QString AddSourceDialog::htmlBridgeUrl() const
{
    return m_htmlBridgeCheck->isChecked() ? kDefaultHtmlBridgeUrl : QString();
}
QString AddSourceDialog::scriptPath() const { return m_scriptPathEdit->text(); }

void AddSourceDialog::browseForScript()
{
    // The examples that came with the program are the likely first pick.
    const QString examples = findAppFile(QStringLiteral("examples"));
    const QString path = QFileDialog::getOpenFileName(this, tr("Wybierz skrypt pluginu"),
                                                        QDir(examples).exists() ? examples : QString(),
                                                        tr("Skrypty Python (*.py)"));
    if (path.isEmpty())
        return;
    m_scriptPathEdit->setText(path);
    invalidateDetectedFlavor();
}

void AddSourceDialog::onHtmlBridgeToggled(bool checked)
{
    // The bridge only scrapes Gelbooru-style HTML; lock the flavor to that.
    m_flavorCombo->setEnabled(!checked);
    if (checked) {
        const int gelbooruIndex = m_flavorCombo->findText(QStringLiteral("gelbooru"));
        if (gelbooruIndex >= 0)
            m_flavorCombo->setCurrentIndex(gelbooruIndex);
    }
    invalidateDetectedFlavor();
}

void AddSourceDialog::startTest()
{
    if (sourceName().isEmpty() || normalizedBaseUrl().isEmpty()) {
        m_resultLabel->setText(tr("Podaj nazwę i adres URL."));
        return;
    }

    invalidateDetectedFlavor();
    m_logEdit->clear();

    SourceDetector::Request request;
    request.url = normalizedBaseUrl();
    // Index, not the label: keying control flow off translated text is fragile.
    request.flavor = m_flavorCombo->currentIndex() == 0 ? QString() : m_flavorCombo->currentText();
    request.apiKey = apiKey();
    request.userId = userId();
    request.cookie = cookie();
    request.userAgent = userAgent();
    request.htmlBridgeUrl = htmlBridgeUrl();
    request.scriptPath = scriptPath();

    setInputsEnabled(false);
    m_resultLabel->setText(tr("Testowanie..."));
    m_detector->start(request);
}

void AddSourceDialog::onProbing(const QString &flavor, int index, int total, const QString &tag)
{
    m_resultLabel->setText(tag.isEmpty()
        ? tr("Próbuję mechanizmu: %1 (%2/%3)...").arg(flavor).arg(index).arg(total)
        : tr("Próbuję mechanizmu: %1 z tagiem \"%2\" (%3/%4)...").arg(flavor, tag).arg(index).arg(total));
}

void AddSourceDialog::onDetectionFinished(const SourceDetector::Result &result)
{
    setInputsEnabled(true);

    if (result.found) {
        m_detectedFlavor = result.flavor;
        QString text = tr("Wykryto mechanizm: %1 - test OK (%2 postów).")
            .arg(m_detectedFlavor).arg(result.postCount);
        // Saved as tested: the base address, not the post or search typed.
        if (result.baseUrl != normalizedBaseUrl()) {
            m_urlEdit->setText(result.baseUrl);
            text += QLatin1Char(' ') + tr("Adres ustawiony na %1.").arg(result.baseUrl);
        }
        m_resultLabel->setText(text);
        m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        return;
    }

    if (m_flavorCombo->currentText() == QStringLiteral("python") && m_flavorCombo->currentIndex() != 0) {
        m_resultLabel->setText(tr("Skrypt pluginu nie zadziałał - sprawdź log poniżej."));
        return;
    }

    switch (result.failure) {
    case SourceDetector::Failure::InvalidUrl:
        m_resultLabel->setText(tr("To nie jest adres strony - wpisz np. https://example-booru.com."));
        break;
    case SourceDetector::Failure::Blocked:
        m_resultLabel->setText(tr("Strona pokazuje ochronę przed botami (%1) zamiast treści. Otwórz ją w "
                                  "przeglądarce, przejdź weryfikację, skopiuj z przeglądarki cookie i "
                                  "User-Agent do pól powyżej i testuj ponownie.").arg(result.blocker));
        break;
    case SourceDetector::Failure::NeedsCredentials:
        m_resultLabel->setText(tr("Mechanizm %1 odpowiada, ale wymaga klucza: wpisz API key i User ID z "
                                  "ustawień konta na tej stronie i testuj ponownie.").arg(result.credentialsFlavor));
        break;
    case SourceDetector::Failure::RateLimited:
        m_resultLabel->setText(tr("Strona odpowiada, że dostaje za dużo zapytań (HTTP 429). Odczekaj minutę "
                                  "i testuj ponownie."));
        break;
    case SourceDetector::Failure::NeedsHtmlBridge:
        m_resultLabel->setText(tr("To booru na silniku Gelbooru bez API. Zaznacz \"Ta strona nie ma API "
                                  "(dapi)\", uruchom most (python3 %1) i testuj ponownie.")
                                   .arg(QDir::toNativeSeparators(findAppFile(QStringLiteral("tools/html_scrape_bridge.py")))));
        break;
    default:
        m_resultLabel->setText(tr("Żaden znany mechanizm nie zadziałał pod tym adresem. Co odpowiedziała "
                                  "strona, widać w logu poniżej."));
        break;
    }
}

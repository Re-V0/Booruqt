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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFileDialog>
#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

namespace {
constexpr int kProbeTimeoutMs = 8000;
// Address of the manually-started html_scrape_bridge.py.
const QString kDefaultHtmlBridgeUrl = QStringLiteral("http://127.0.0.1:8765");

// Tried in order until one parses: an empty query, then "all" (what the
// Gelbooru-engine forks use for "everything"), then a common anime tag as a
// last resort for sites that answer nothing to either.
const QStringList &probeTags()
{
    static const QStringList tags = {QString(), QStringLiteral("all"), QStringLiteral("1girl")};
    return tags;
}

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
    : QDialog(parent), m_probeNetwork(new QNetworkAccessManager(this))
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
    const QString path = QFileDialog::getOpenFileName(this, tr("Wybierz skrypt pluginu"),
                                                        QString(), tr("Skrypty Python (*.py)"));
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
    const QString url = normalizedBaseUrl();
    if (sourceName().isEmpty() || url.isEmpty()) {
        m_resultLabel->setText(tr("Podaj nazwę i adres URL."));
        return;
    }

    invalidateDetectedFlavor();
    m_logEdit->clear();

    // Index, not the label: keying control flow off translated text is fragile.
    const QString chosen = m_flavorCombo->currentText();
    if (m_flavorCombo->currentIndex() == 0) {
        m_flavorsToTry = SourceManager::knownFlavors();
        // Auto-detect skips "python" normally (needs a script) - include it if one is set.
        if (!scriptPath().isEmpty())
            m_flavorsToTry.append(QStringLiteral("python"));
    } else {
        m_flavorsToTry = QStringList{chosen};
    }
    m_tryIndex = 0;
    m_probePass = 0;

    setInputsEnabled(false);
    m_resultLabel->setText(tr("Testowanie..."));
    tryNextFlavor();
}

void AddSourceDialog::tryNextFlavor()
{
    if (m_tryIndex >= m_flavorsToTry.size()) {
        if (m_probePass + 1 < probeTags().size()) {
            ++m_probePass;
            m_tryIndex = 0;
            m_logEdit->appendPlainText(QStringLiteral("(empty query didn't work anywhere - retrying with tag \"%1\")").arg(probeTags().at(m_probePass)));
        } else {
            setInputsEnabled(true);
            m_resultLabel->setText(m_flavorsToTry == QStringList{QStringLiteral("python")}
                ? tr("Skrypt pluginu nie zadziałał - sprawdź log poniżej.")
                : tr("Żaden znany mechanizm nie zadziałał pod tym adresem. Sprawdź URL "
                     "(musi wskazywać na stronę główną booru, nie na konkretny post)."));
            return;
        }
    }

    const QString flavor = m_flavorsToTry.at(m_tryIndex);
    m_probeSource = SourceManager::createSource(QStringLiteral("__probe__"), flavor, baseUrl(),
                                                 apiKey(), userId(), cookie(), userAgent(), htmlBridgeUrl(),
                                                 scriptPath());
    if (!m_probeSource) {
        ++m_tryIndex;
        tryNextFlavor();
        return;
    }

    m_resultLabel->setText(m_probePass == 0
        ? tr("Próbuję mechanizmu: %1 (%2/%3)...").arg(flavor).arg(m_tryIndex + 1).arg(m_flavorsToTry.size())
        : tr("Próbuję mechanizmu: %1 z tagiem \"%2\" (%3/%4)...")
              .arg(flavor, probeTags().at(m_probePass)).arg(m_tryIndex + 1).arg(m_flavorsToTry.size()));

    // Empty query = universal smoke test; one parseable post confirms the flavor.
    const QString probeTag = probeTags().at(m_probePass);
    const QSharedPointer<ISource> source = m_probeSource;

    if (!source->mayBlock()) {
        sendProbeRequest(source->buildSearchRequest(probeTag, 1, 5), flavor);
        return;
    }

    // A plugin builds its URL by running python3; that would freeze the dialog.
    auto *watcher = new QFutureWatcher<QNetworkRequest>(this);
    connect(watcher, &QFutureWatcher<QNetworkRequest>::finished, this, [this, watcher, flavor]() {
        watcher->deleteLater();
        sendProbeRequest(watcher->result(), flavor);
    });
    watcher->setFuture(QtConcurrent::run([source, probeTag]() {
        return source->buildSearchRequest(probeTag, 1, 5);
    }));
}

void AddSourceDialog::sendProbeRequest(QNetworkRequest request, const QString &flavor)
{
    if (!request.url().isValid() || request.url().isEmpty()) {
        m_logEdit->appendPlainText(QStringLiteral("[%1] no usable request was built").arg(flavor));
        ++m_tryIndex;
        tryNextFlavor();
        return;
    }

    request.setTransferTimeout(kProbeTimeoutMs);
    const QString shownUrl = NetworkLog::redactedUrl(request.url());
    m_logEdit->appendPlainText(QStringLiteral("[%1] GET %2").arg(flavor, shownUrl));
    NetworkLog::instance().log(QStringLiteral("(source test) GET %1").arg(shownUrl));

    QNetworkReply *reply = m_probeNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, flavor]() {
        onProbeReplyFinished(reply, flavor);
    });
}

void AddSourceDialog::onProbeReplyFinished(QNetworkReply *reply, const QString &flavor)
{
    reply->deleteLater();

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        handleProbeResult(flavor, httpStatus, {}, reply->errorString());
        return;
    }

    const QByteArray data = reply->readAll();
    const QSharedPointer<ISource> source = m_probeSource;

    if (!source->mayBlock()) {
        const QVector<Post> posts = source->parseSearchResponse(data);
        handleProbeResult(flavor, httpStatus, posts,
                          posts.isEmpty() ? QString::fromUtf8(data.left(200)).simplified() : QString());
        return;
    }

    auto *watcher = new QFutureWatcher<QVector<Post>>(this);
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this, [this, watcher, flavor, httpStatus, data]() {
        watcher->deleteLater();
        const QVector<Post> posts = watcher->result();
        handleProbeResult(flavor, httpStatus, posts,
                          posts.isEmpty() ? QString::fromUtf8(data.left(200)).simplified() : QString());
    });
    watcher->setFuture(QtConcurrent::run([source, data]() {
        return source->parseSearchResponse(data);
    }));
}

void AddSourceDialog::handleProbeResult(const QString &flavor, int httpStatus, const QVector<Post> &posts,
                                        const QString &snippet)
{
    m_logEdit->appendPlainText(QStringLiteral("  -> HTTP %1, %2 posts parsed%3")
        .arg(httpStatus).arg(posts.size())
        .arg(snippet.isEmpty() ? QString() : QStringLiteral(" | %1").arg(snippet)));

    if (!posts.isEmpty()) {
        m_detectedFlavor = flavor;
        setInputsEnabled(true);
        m_resultLabel->setText(tr("Wykryto mechanizm: %1 - test OK (%2 postów).")
                                   .arg(m_detectedFlavor).arg(posts.size()));
        m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        return;
    }

    ++m_tryIndex;
    tryNextFlavor();
}

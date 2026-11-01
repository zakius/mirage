/*
 * Fooyin
 * Copyright © 2026, zakius <zakius.rolkash@gmail.com>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "subsonicpage.h"

#include "subsonicprovider.h"

#include <core/library/librarymanager.h>
#include <core/library/musiclibrary.h>
#include <utils/settings/settingsmanager.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

constexpr auto PageId = "Fooyin.Page.Library.Subsonic";

namespace Fooyin::Subsonic {
class SubsonicPageWidget : public SettingsPageWidget
{
    Q_OBJECT

public:
    SubsonicPageWidget(SubsonicProvider* provider, LibraryManager* libraryManager, MusicLibrary* library);

    void load() override { }
    void apply() override { }
    void reset() override { }

private:
    void populateServers();
    void serverChanged();
    void saveServer();
    void finishSave(Account account, const QString& existingPath);
    void updateScrobbleHint();

    SubsonicProvider* m_provider;
    LibraryManager* m_libraryManager;
    MusicLibrary* m_library;

    QComboBox* m_server;
    QLineEdit* m_name;
    QLineEdit* m_url;
    QLineEdit* m_username;
    QLineEdit* m_password;
    QCheckBox* m_scrobble;
    QLabel* m_scrobbleHint;
    //! Set once the user changes m_scrobble, so an untouched box keeps following the server.
    bool m_scrobbleTouched{false};
    QPushButton* m_save;
    QLabel* m_status;
};

SubsonicPageWidget::SubsonicPageWidget(SubsonicProvider* provider, LibraryManager* libraryManager,
                                       MusicLibrary* library)
    : m_provider{provider}
    , m_libraryManager{libraryManager}
    , m_library{library}
    , m_server{new QComboBox(this)}
    , m_name{new QLineEdit(this)}
    , m_url{new QLineEdit(this)}
    , m_username{new QLineEdit(this)}
    , m_password{new QLineEdit(this)}
    , m_scrobble{new QCheckBox(tr("Also scrobble this server's plays with mirage's scrobbler"), this)}
    , m_scrobbleHint{new QLabel(this)}
    , m_save{new QPushButton(this)}
    , m_status{new QLabel(this)}
{
    m_server->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_url->setPlaceholderText(u"https://music.example.org"_s);
    m_password->setEchoMode(QLineEdit::Password);
    m_status->setWordWrap(true);
    m_scrobbleHint->setWordWrap(true);

    auto* form = new QFormLayout();
    form->addRow(tr("Server") + u":"_s, m_server);
    form->addRow(tr("Name") + u":"_s, m_name);
    form->addRow(tr("Server URL") + u":"_s, m_url);
    form->addRow(tr("Username") + u":"_s, m_username);
    form->addRow(tr("Password") + u":"_s, m_password);

    auto* hint = new QLabel(tr("The server's tracks are added to the library. Refresh or remove the server on the "
                               "Library page."),
                            this);
    hint->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(form);
    layout->addWidget(m_scrobble);
    layout->addWidget(m_scrobbleHint);
    layout->addWidget(m_save, 0, Qt::AlignLeft);
    layout->addWidget(m_status);
    layout->addStretch();

    populateServers();

    QObject::connect(m_server, &QComboBox::currentIndexChanged, this, &SubsonicPageWidget::serverChanged);
    QObject::connect(m_save, &QPushButton::clicked, this, &SubsonicPageWidget::saveServer);
    QObject::connect(m_scrobble, &QCheckBox::clicked, this, [this]() { m_scrobbleTouched = true; });
    QObject::connect(m_provider, &SubsonicProvider::serverInfoChanged, this, [this](const QString& libraryPath) {
        if(libraryPath == m_server->currentData().toString()) {
            updateScrobbleHint();
        }
    });
}

void SubsonicPageWidget::populateServers()
{
    const QSignalBlocker blocker{m_server};
    const QString current = m_server->currentData().toString();

    m_server->clear();
    m_server->addItem(tr("New server"));
    for(const auto& [id, library] : m_libraryManager->allLibraries()) {
        if(m_provider->isProviderPath(library.path)) {
            m_server->addItem(library.name, library.path);
        }
    }

    m_server->setCurrentIndex(std::max(0, m_server->findData(current)));
    serverChanged();
}

void SubsonicPageWidget::serverChanged()
{
    const QString path = m_server->currentData().toString();
    const auto account = m_provider->accountForPath(path);

    m_name->setText(path.isEmpty() ? QString{} : m_server->currentText());
    m_url->setText(account ? account->url.toString() : QString{});
    m_username->setText(account ? account->username : QString{});
    m_password->clear();
    m_password->setPlaceholderText(path.isEmpty() ? QString{} : tr("Unchanged"));
    m_save->setText(path.isEmpty() ? tr("Add Server") : tr("Save"));
    m_status->clear();
    m_scrobbleTouched = false;
    updateScrobbleHint();
}

void SubsonicPageWidget::updateScrobbleHint()
{
    const QString path = m_server->currentData().toString();
    const auto account = m_provider->accountForPath(path);
    const auto server  = m_provider->serverScrobbles(path);

    if(!m_scrobbleTouched) {
        m_scrobble->setChecked(scrobblesLocally(account ? account->scrobble : std::nullopt, server));
    }
    if(path.isEmpty()) {
        m_scrobbleHint->clear();
    }
    else if(!server) {
        m_scrobbleHint->setText(tr("The server didn't say whether it scrobbles your plays to Last.fm."));
    }
    else {
        m_scrobbleHint->setText(*server ? tr("The server says it scrobbles your plays to Last.fm.")
                                        : tr("The server says it doesn't scrobble your plays to Last.fm."));
    }
}

void SubsonicPageWidget::saveServer()
{
    const QString existingPath = m_server->currentData().toString();
    const auto existing        = m_provider->accountForPath(existingPath);

    Account account{.url      = QUrl::fromUserInput(m_url->text().trimmed()),
                    .username = m_username->text().trimmed(),
                    .password = m_password->text()};
    if(account.password.isEmpty() && existing) {
        account.password = existing->password;
    }
    if(!account.url.isValid() || account.username.isEmpty()) {
        m_status->setText(tr("Enter a server URL and a username."));
        return;
    }

    m_save->setEnabled(false);
    m_status->setText(tr("Connecting…"));

    m_provider->call(
        account, u"ping"_s, {},
        [this, self = QPointer{this}, account, existingPath](std::optional<QJsonObject> response, ApiError error) {
            if(!self) {
                return;
            }
            m_save->setEnabled(true);

            if(!response) {
                m_status->setText(tr("Connection failed: %1").arg(error.message));
                return;
            }
            // The stored choice depends on what the server says it does
            const QString path = existingPath.isEmpty() ? SubsonicProvider::libraryPath(account) : existingPath;
            m_provider->fetchServerInfo(path, account, [this, self, account, existingPath]() {
                if(self) {
                    finishSave(account, existingPath);
                }
            });
        });
}

void SubsonicPageWidget::finishSave(Account account, const QString& existingPath)
{
    // A library path identifies the library, so it stays the same when the server's address changes
    const QString path = existingPath.isEmpty() ? SubsonicProvider::libraryPath(account) : existingPath;

    // Only a choice that differs from the server's default is stored, so an untouched account follows the server
    const bool followed = scrobblesLocally(std::nullopt, m_provider->serverScrobbles(path));
    if(m_scrobbleTouched) {
        account.scrobble = m_scrobble->isChecked() == followed ? std::nullopt : std::optional{m_scrobble->isChecked()};
    }
    else if(const auto existing = m_provider->accountForPath(existingPath)) {
        account.scrobble = existing->scrobble;
    }
    if(!m_provider->saveAccount(path, account)) {
        m_status->setText(tr("Failed to save the account."));
        return;
    }

    const QString name = m_name->text().trimmed();
    m_password->clear();

    if(existingPath.isEmpty()) {
        if(m_libraryManager->addLibrary(path, name.isEmpty() ? account.url.host() : name) < 0) {
            m_status->setText(tr("This server is already a library. Select it above to edit it."));
            return;
        }
        m_status->setText(tr("Added. The server's tracks are being added to the library."));
    }
    else {
        const auto library = m_libraryManager->findLibraryByPath(path);
        if(library && !name.isEmpty() && name != library->name) {
            m_libraryManager->renameLibrary(library->id, name);
        }
        if(library) {
            m_library->refresh(*library);
        }
        m_status->setText(tr("Saved. The library is being refreshed."));
    }

    populateServers();
    m_server->setCurrentIndex(std::max(0, m_server->findData(path)));
}

SubsonicPage::SubsonicPage(SubsonicProvider* provider, LibraryManager* libraryManager, MusicLibrary* library,
                           SettingsManager* settings, QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(PageId);
    setName(tr("Subsonic"));
    setCategory({tr("Library"), tr("Subsonic")});
    setWidgetCreator(
        [provider, libraryManager, library] { return new SubsonicPageWidget(provider, libraryManager, library); });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicpage.cpp"
#include "subsonicpage.moc"

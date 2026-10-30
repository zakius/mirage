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
#include <utils/settings/settingsmanager.h>

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
    SubsonicPageWidget(SubsonicProvider* provider, LibraryManager* libraryManager);

    void load() override { }
    void apply() override { }
    void reset() override { }

private:
    void addServer();

    SubsonicProvider* m_provider;
    LibraryManager* m_libraryManager;

    QLineEdit* m_name;
    QLineEdit* m_url;
    QLineEdit* m_username;
    QLineEdit* m_password;
    QPushButton* m_add;
    QLabel* m_status;
};

SubsonicPageWidget::SubsonicPageWidget(SubsonicProvider* provider, LibraryManager* libraryManager)
    : m_provider{provider}
    , m_libraryManager{libraryManager}
    , m_name{new QLineEdit(this)}
    , m_url{new QLineEdit(this)}
    , m_username{new QLineEdit(this)}
    , m_password{new QLineEdit(this)}
    , m_add{new QPushButton(tr("Add Server"), this)}
    , m_status{new QLabel(this)}
{
    m_url->setPlaceholderText(u"https://music.example.org"_s);
    m_password->setEchoMode(QLineEdit::Password);
    m_status->setWordWrap(true);

    auto* form = new QFormLayout();
    form->addRow(tr("Name") + u":"_s, m_name);
    form->addRow(tr("Server URL") + u":"_s, m_url);
    form->addRow(tr("Username") + u":"_s, m_username);
    form->addRow(tr("Password") + u":"_s, m_password);

    auto* hint = new QLabel(tr("The server's tracks are added to the library. Manage, refresh or remove the server "
                               "on the Library page."),
                            this);
    hint->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(form);
    layout->addWidget(m_add, 0, Qt::AlignLeft);
    layout->addWidget(m_status);
    layout->addStretch();

    QObject::connect(m_add, &QPushButton::clicked, this, &SubsonicPageWidget::addServer);
}

void SubsonicPageWidget::addServer()
{
    const Account account{.url      = QUrl::fromUserInput(m_url->text().trimmed()),
                          .username = m_username->text().trimmed(),
                          .password = m_password->text()};
    if(!account.url.isValid() || account.username.isEmpty()) {
        m_status->setText(tr("Enter a server URL and a username."));
        return;
    }

    m_add->setEnabled(false);
    m_status->setText(tr("Connecting…"));

    m_provider->call(account, u"ping"_s, {},
                     [this, self = QPointer{this}, account](std::optional<QJsonObject> response, QString error) {
                         if(!self) {
                             return;
                         }
                         m_add->setEnabled(true);

                         if(!response) {
                             m_status->setText(tr("Connection failed: %1").arg(error));
                             return;
                         }

                         const QString path = SubsonicProvider::libraryPath(account);
                         if(!m_provider->saveAccount(path, account)) {
                             m_status->setText(tr("Failed to save the account."));
                             return;
                         }

                         const QString name = m_name->text().trimmed();
                         if(m_libraryManager->addLibrary(path, name.isEmpty() ? account.url.host() : name) < 0) {
                             m_status->setText(tr("Updated the account of the existing library."));
                             return;
                         }

                         m_status->setText(tr("Added %1. Its tracks are being added to the library.").arg(path));
                         m_password->clear();
                     });
}

SubsonicPage::SubsonicPage(SubsonicProvider* provider, LibraryManager* libraryManager, SettingsManager* settings,
                           QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(PageId);
    setName(tr("Subsonic"));
    setCategory({tr("Library"), tr("Subsonic")});
    setWidgetCreator([provider, libraryManager] { return new SubsonicPageWidget(provider, libraryManager); });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicpage.cpp"
#include "subsonicpage.moc"

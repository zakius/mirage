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

#include "subsonicprovider.h"

#include "rangestreamdevice.h"

#include <core/network/networkaccessmanager.h>

#include <QJsonArray>
#include <QLoggingCategory>

#include <deque>

Q_LOGGING_CATEGORY(SUBSONIC, "fy.subsonic")

using namespace Qt::StringLiterals;

constexpr auto Scheme   = "subsonic";
constexpr auto PageSize = 500;

constexpr auto UrlField      = "Url";
constexpr auto UsernameField = "Username";
constexpr auto PasswordField = "Password";

namespace {
QString accountKey(const QString& libraryPath)
{
    return u"Subsonic/"_s + libraryPath;
}

std::vector<QString> accountFields()
{
    return {QString::fromLatin1(UrlField), QString::fromLatin1(UsernameField), QString::fromLatin1(PasswordField)};
}
} // namespace

namespace Fooyin::Subsonic {
struct SyncState
{
    QString libraryPath;
    Account account;
    LibraryProvider::FetchCallback callback;
    TrackList tracks;
    std::deque<QString> albumIds;
};

SubsonicProvider::SubsonicProvider(std::shared_ptr<NetworkAccessManager> network, DbConnectionPoolPtr dbPool,
                                   QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_accountStore{std::move(dbPool)}
{ }

QString SubsonicProvider::scheme() const
{
    return QString::fromLatin1(Scheme);
}

void SubsonicProvider::fetchTracks(const LibraryInfo& library, FetchCallback callback)
{
    const auto account = accountForPath(library.path);
    if(!account) {
        qCWarning(SUBSONIC) << "No account for library" << library.name;
        callback({});
        return;
    }

    auto state         = std::make_shared<SyncState>();
    state->libraryPath = library.path;
    state->account     = *account;
    state->callback    = std::move(callback);
    fetchSongs(state, 0);
}

RemoteStreamSource SubsonicProvider::createStreamSource(const QUrl& url) const
{
    const QString path = url.toString();
    const auto account = accountForPath(path);
    if(!account) {
        return {};
    }

    QUrlQuery params;
    params.addQueryItem(u"id"_s, songId(path));

    RemoteStreamSource source;
    source.device = std::make_unique<RangeStreamDevice>(m_network, apiUrl(*account, u"stream"_s, params));
    return source;
}

QUrl SubsonicProvider::resolveUrl(const QUrl& url) const
{
    const QString path = url.toString();
    const auto account = accountForPath(path);
    if(!account) {
        return {};
    }

    QUrlQuery params;
    params.addQueryItem(u"id"_s, coverId(path));
    return apiUrl(*account, u"getCoverArt"_s, params);
}

bool SubsonicProvider::reportsPlays() const
{
    return true;
}

QString SubsonicProvider::libraryPath(const Account& account)
{
    QUrl url;
    url.setScheme(QString::fromLatin1(Scheme));
    url.setUserName(account.username);
    url.setHost(account.url.host());
    url.setPort(account.url.port());
    url.setPath(account.url.path());
    return url.toString(QUrl::StripTrailingSlash);
}

bool SubsonicProvider::isProviderPath(const QString& path) const
{
    return accountForPath(path).has_value();
}

void SubsonicProvider::loadAccount(const QString& libraryPath)
{
    const auto values = m_accountStore.load(accountKey(libraryPath), accountFields());
    if(!values || !values->contains(QString::fromLatin1(UrlField))) {
        qCWarning(SUBSONIC) << "No stored account for" << libraryPath;
        return;
    }

    const Account account{.url      = QUrl{values->at(QString::fromLatin1(UrlField))},
                          .username = values->at(QString::fromLatin1(UsernameField)),
                          .password = values->at(QString::fromLatin1(PasswordField))};

    const std::scoped_lock lock{m_mutex};
    m_accounts[libraryPath] = account;
}

bool SubsonicProvider::saveAccount(const QString& libraryPath, const Account& account)
{
    const AccountStore::Values values{{QString::fromLatin1(UrlField), account.url.toString()},
                                      {QString::fromLatin1(UsernameField), account.username},
                                      {QString::fromLatin1(PasswordField), account.password}};
    if(!m_accountStore.save(accountKey(libraryPath), values)) {
        return false;
    }

    const std::scoped_lock lock{m_mutex};
    m_accounts[libraryPath] = account;
    return true;
}

void SubsonicProvider::removeAccount(const QString& libraryPath)
{
    std::ignore = m_accountStore.remove(accountKey(libraryPath), accountFields());

    const std::scoped_lock lock{m_mutex};
    m_accounts.erase(libraryPath);
}

void SubsonicProvider::call(const QString& path, const QString& method, const QUrlQuery& params,
                            ResponseCallback callback)
{
    if(const auto account = accountForPath(path)) {
        call(*account, method, params, std::move(callback));
    }
    else {
        callback({}, tr("No account for %1").arg(path));
    }
}

void SubsonicProvider::call(const Account& account, const QString& method, const QUrlQuery& params,
                            ResponseCallback callback)
{
    request(m_network.get(), account, method, params, this, std::move(callback));
}

std::optional<Account> SubsonicProvider::accountForPath(const QString& path) const
{
    const std::scoped_lock lock{m_mutex};
    for(const auto& [libraryPath, account] : m_accounts) {
        if(path == libraryPath || path.startsWith(libraryPath + '/'_L1)) {
            return account;
        }
    }
    return {};
}

void SubsonicProvider::finishSync(const std::shared_ptr<SyncState>& state)
{
    qCInfo(SUBSONIC) << "Fetched" << state->tracks.size() << "songs from" << state->libraryPath;
    Q_EMIT tracksFetched(state->libraryPath);
    state->callback(std::move(state->tracks));
}

void SubsonicProvider::fetchSongs(const std::shared_ptr<SyncState>& state, int offset)
{
    QUrlQuery params;
    // Servers commonly accept a literal pair of quotes as the "match everything" query
    params.addQueryItem(u"query"_s, uR"("")"_s);
    params.addQueryItem(u"artistCount"_s, u"0"_s);
    params.addQueryItem(u"albumCount"_s, u"0"_s);
    params.addQueryItem(u"songCount"_s, QString::number(PageSize));
    params.addQueryItem(u"songOffset"_s, QString::number(offset));

    call(state->account, u"search3"_s, params,
         [this, state, offset](std::optional<QJsonObject> response, QString error) {
             if(!response) {
                 if(offset == 0) {
                     // Not every server lists all songs for an empty query
                     qCInfo(SUBSONIC) << "search3 failed, listing albums instead:" << error;
                     fetchAlbumList(state, 0);
                 }
                 else {
                     qCWarning(SUBSONIC) << "Failed to list songs:" << error;
                     state->callback({});
                 }
                 return;
             }

             const QJsonArray songs = toArray(response->value("searchResult3"_L1).toObject().value("song"_L1));
             for(const auto& song : songs) {
                 state->tracks.push_back(songToTrack(song.toObject(), state->libraryPath));
             }

             if(songs.size() == PageSize) {
                 fetchSongs(state, offset + PageSize);
             }
             else if(state->tracks.empty()) {
                 fetchAlbumList(state, 0);
             }
             else {
                 finishSync(state);
             }
         });
}

void SubsonicProvider::fetchAlbumList(const std::shared_ptr<SyncState>& state, int offset)
{
    QUrlQuery params;
    params.addQueryItem(u"type"_s, u"alphabeticalByName"_s);
    params.addQueryItem(u"size"_s, QString::number(PageSize));
    params.addQueryItem(u"offset"_s, QString::number(offset));

    call(state->account, u"getAlbumList2"_s, params,
         [this, state, offset](std::optional<QJsonObject> response, QString error) {
             if(!response) {
                 qCWarning(SUBSONIC) << "Failed to list albums:" << error;
                 state->callback({});
                 return;
             }

             const QJsonArray albums = toArray(response->value("albumList2"_L1).toObject().value("album"_L1));
             for(const auto& album : albums) {
                 state->albumIds.push_back(album.toObject().value("id"_L1).toString());
             }

             if(albums.size() == PageSize) {
                 fetchAlbumList(state, offset + PageSize);
             }
             else {
                 fetchNextAlbum(state);
             }
         });
}

void SubsonicProvider::fetchNextAlbum(const std::shared_ptr<SyncState>& state)
{
    if(state->albumIds.empty()) {
        finishSync(state);
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"id"_s, state->albumIds.front());
    state->albumIds.pop_front();

    call(state->account, u"getAlbum"_s, params, [this, state](std::optional<QJsonObject> response, QString error) {
        if(!response) {
            qCWarning(SUBSONIC) << "Failed to fetch album:" << error;
            state->callback({});
            return;
        }

        for(const auto& song : toArray(response->value("album"_L1).toObject().value("song"_L1))) {
            state->tracks.push_back(songToTrack(song.toObject(), state->libraryPath));
        }
        fetchNextAlbum(state);
    });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicprovider.cpp"

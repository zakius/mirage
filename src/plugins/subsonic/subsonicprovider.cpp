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
#include <set>
#include <ranges>

Q_LOGGING_CATEGORY(SUBSONIC, "fy.subsonic")

using namespace Qt::StringLiterals;

constexpr auto Scheme   = "subsonic";
constexpr auto PageSize = 500;
//! Songs asked for by a library search; servers such as oasis add songs the library does not hold.
constexpr auto SearchSongCount = 50;
//! Subsonic error code for a missing or empty required parameter.
constexpr int MissingParameterCode = 10;

constexpr auto UrlField      = "Url";
constexpr auto UsernameField = "Username";
constexpr auto PasswordField = "Password";
//! "1" or "0" where the user chose whether mirage's scrobbler also scrobbles; absent or empty follows the server.
constexpr auto ScrobbleField = "Scrobble";

namespace {
QString accountKey(const QString& libraryPath)
{
    return u"Subsonic/"_s + libraryPath;
}

std::vector<QString> accountFields()
{
    return {QString::fromLatin1(UrlField), QString::fromLatin1(UsernameField), QString::fromLatin1(PasswordField),
            QString::fromLatin1(ScrobbleField)};
}

//! Adds `romanization=both` for servers with the extension, so songs carry their romanized names next to the originals.
void addRomanization(QUrlQuery& params, bool romanization)
{
    if(romanization) {
        params.addQueryItem(u"romanization"_s, u"both"_s);
    }
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
    //! Songs and albums listed so far, so a page that repeats them ends the listing.
    std::set<QString> songIds;
    std::set<QString> listedAlbums;
    bool romanization{false};
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
    fetchServerInfo(library.path, *account, [this, state]() {
        state->romanization = hasRomanization(state->libraryPath);
        fetchSongs(state, 0);
    });
}

void SubsonicProvider::searchTracks(const LibraryInfo& library, const QString& query, FetchCallback callback)
{
    const auto account = accountForPath(library.path);
    if(!account) {
        callback({});
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"query"_s, query);
    params.addQueryItem(u"artistCount"_s, u"0"_s);
    params.addQueryItem(u"albumCount"_s, u"0"_s);
    params.addQueryItem(u"songCount"_s, QString::number(SearchSongCount));
    addRomanization(params, hasRomanization(library.path));

    call(*account, u"search3"_s, params,
         [libraryPath = library.path, callback = std::move(callback)](std::optional<QJsonObject> response,
                                                                       const ApiError& error) {
             if(!response) {
                 qCWarning(SUBSONIC) << "Failed to search:" << error.message;
                 callback({});
                 return;
             }

             TrackList tracks;
             for(const auto& song : toArray(response->value("searchResult3"_L1).toObject().value("song"_L1))) {
                 Track track = songToTrack(song.toObject(), libraryPath);
                 track.generateHash();
                 tracks.push_back(track);
             }
             callback(tracks);
         });
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
    const QString path = url.toString(QUrl::RemoveQuery);
    const auto account = accountForPath(path);
    if(!account) {
        return {};
    }

    // `<library path>/rest/<method>?<params>` signs an API call, for example for the lyrics plugin
    if(const auto index = path.lastIndexOf("/rest/"_L1); index >= 0) {
        return apiUrl(*account, path.sliced(index + 6), QUrlQuery{url});
    }

    QUrlQuery params;
    params.addQueryItem(u"id"_s, coverId(path));
    return apiUrl(*account, u"getCoverArt"_s, params);
}

bool SubsonicProvider::reportsPlays(const Track& track) const
{
    const QString libraryPath = libraryPathFor(track.filepath());
    const auto account        = accountForPath(libraryPath);
    return account && !scrobblesLocally(account->scrobble, serverScrobbles(libraryPath));
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

    const auto value = [&values](const char* field) {
        const auto it = values->find(QString::fromLatin1(field));
        return it != values->cend() ? it->second : QString{};
    };
    const QString scrobble = value(ScrobbleField);
    const Account account{.url      = QUrl{value(UrlField)},
                          .username = value(UsernameField),
                          .password = value(PasswordField),
                          .scrobble = scrobble.isEmpty() ? std::nullopt : std::optional{scrobble == "1"_L1}};

    {
        const std::scoped_lock lock{m_mutex};
        m_accounts[libraryPath] = account;
    }
    fetchServerInfo(libraryPath, account);
}

bool SubsonicProvider::saveAccount(const QString& libraryPath, const Account& account)
{
    const AccountStore::Values values{{QString::fromLatin1(UrlField), account.url.toString()},
                                      {QString::fromLatin1(UsernameField), account.username},
                                      {QString::fromLatin1(PasswordField), account.password},
                                      {QString::fromLatin1(ScrobbleField),
                                       account.scrobble ? (*account.scrobble ? u"1"_s : u"0"_s) : QString{}}};
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
    m_serverScrobbles.erase(libraryPath);
    m_romanization.erase(libraryPath);
    m_formPost.erase(libraryPath);
    m_uploadExtension.erase(libraryPath);
    m_uploadRole.erase(libraryPath);
}

void SubsonicProvider::fetchServerInfo(const QString& libraryPath, const Account& account, std::function<void()> done)
{
    // Both answers are awaited, so `done` runs once
    auto pending = std::make_shared<int>(2);
    auto finish  = [this, libraryPath, pending, done = std::move(done)]() {
        if(--*pending == 0) {
            Q_EMIT serverInfoChanged(libraryPath);
            if(done) {
                done();
            }
        }
    };

    QUrlQuery user;
    user.addQueryItem(u"username"_s, account.username);
    call(account, u"getUser"_s, user, [this, libraryPath, finish](std::optional<QJsonObject> response, ApiError) {
        if(response) {
            const std::scoped_lock lock{m_mutex};
            if(uploadRole(*response)) {
                m_uploadRole.emplace(libraryPath);
            }
            else {
                m_uploadRole.erase(libraryPath);
            }
            if(const auto scrobbles = Subsonic::serverScrobbles(*response)) {
                m_serverScrobbles[libraryPath] = *scrobbles;
            }
            else {
                m_serverScrobbles.erase(libraryPath);
            }
        }
        finish();
    });

    call(account, u"getOpenSubsonicExtensions"_s, {},
         [this, libraryPath, finish](std::optional<QJsonObject> response, ApiError error) {
             // Servers without OpenSubsonic answer with an error, so only network errors keep what was known
             if(response || error.code >= 0) {
                 const std::scoped_lock lock{m_mutex};
                 if(response && hasExtension(*response, u"romanization"_s, 1)) {
                     m_romanization.emplace(libraryPath);
                 }
                 else {
                     m_romanization.erase(libraryPath);
                 }
                 if(response && hasExtension(*response, u"formPost"_s, 1)) {
                     m_formPost.emplace(libraryPath);
                 }
                 else {
                     m_formPost.erase(libraryPath);
                 }
                 if(response && hasExtension(*response, u"upload"_s, 1)) {
                     m_uploadExtension.emplace(libraryPath);
                 }
                 else {
                     m_uploadExtension.erase(libraryPath);
                 }
             }
             finish();
         });
}

std::optional<bool> SubsonicProvider::serverScrobbles(const QString& libraryPath) const
{
    const std::scoped_lock lock{m_mutex};
    const auto it = m_serverScrobbles.find(libraryPath);
    return it != m_serverScrobbles.cend() ? std::optional{it->second} : std::nullopt;
}

bool SubsonicProvider::hasRomanization(const QString& path) const
{
    const QString libraryPath = libraryPathFor(path);
    const std::scoped_lock lock{m_mutex};
    return m_romanization.contains(libraryPath);
}

bool SubsonicProvider::hasFormPost(const QString& path) const
{
    const QString libraryPath = libraryPathFor(path);
    const std::scoped_lock lock{m_mutex};
    return m_formPost.contains(libraryPath);
}

bool SubsonicProvider::canUpload(const QString& path) const
{
    const QString libraryPath = libraryPathFor(path);
    const std::scoped_lock lock{m_mutex};
    return m_uploadExtension.contains(libraryPath) && m_uploadRole.contains(libraryPath);
}

void SubsonicProvider::call(const QString& path, const QString& method, const QUrlQuery& params,
                            ResponseCallback callback)
{
    if(const auto account = accountForPath(path)) {
        call(*account, method, params, std::move(callback));
    }
    else {
        callback({}, {.message = tr("No account for %1").arg(path)});
    }
}

void SubsonicProvider::callForm(const QString& path, const QString& method, const QUrlQuery& params,
                                ResponseCallback callback)
{
    const auto account = accountForPath(path);
    if(account && hasFormPost(path)) {
        postForm(m_network.get(), *account, method, params, this, std::move(callback));
    }
    else {
        call(path, method, params, std::move(callback));
    }
}

void SubsonicProvider::call(const Account& account, const QString& method, const QUrlQuery& params,
                            ResponseCallback callback)
{
    request(m_network.get(), account, method, params, this, std::move(callback));
}

std::optional<Account> SubsonicProvider::accountForPath(const QString& path) const
{
    const QString libraryPath = libraryPathFor(path);
    const std::scoped_lock lock{m_mutex};
    const auto it = m_accounts.find(libraryPath);
    return it != m_accounts.cend() ? std::optional{it->second} : std::nullopt;
}

QString SubsonicProvider::libraryPathFor(const QString& path) const
{
    const std::scoped_lock lock{m_mutex};
    for(const auto& libraryPath : m_accounts | std::views::keys) {
        if(path == libraryPath || path.startsWith(libraryPath + '/'_L1)) {
            return libraryPath;
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
    addRomanization(params, state->romanization);

    call(state->account, u"search3"_s, params,
         [this, state, offset](std::optional<QJsonObject> response, ApiError error) {
             if(!response) {
                 if(offset == 0 && error.code == MissingParameterCode) {
                     // Not every server lists all songs for an empty query
                     qCInfo(SUBSONIC) << "search3 failed, listing albums instead:" << error.message;
                     fetchAlbumList(state, 0);
                 }
                 else {
                     qCWarning(SUBSONIC) << "Failed to list songs:" << error.message;
                     state->callback({});
                 }
                 return;
             }

             // Servers may cap songCount, so pages are asked until one adds nothing; a server that ignores songOffset
             // sends the same songs again
             bool added{false};
             const QJsonArray songs = toArray(response->value("searchResult3"_L1).toObject().value("song"_L1));
             for(const auto& song : songs) {
                 if(state->songIds.emplace(song.toObject().value("id"_L1).toString()).second) {
                     state->tracks.push_back(songToTrack(song.toObject(), state->libraryPath));
                     added = true;
                 }
             }

             if(added) {
                 fetchSongs(state, offset + static_cast<int>(songs.size()));
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
         [this, state, offset](std::optional<QJsonObject> response, ApiError error) {
             if(!response) {
                 qCWarning(SUBSONIC) << "Failed to list albums:" << error.message;
                 state->callback({});
                 return;
             }

             bool added{false};
             const QJsonArray albums = toArray(response->value("albumList2"_L1).toObject().value("album"_L1));
             for(const auto& album : albums) {
                 const QString id = album.toObject().value("id"_L1).toString();
                 if(state->listedAlbums.emplace(id).second) {
                     state->albumIds.push_back(id);
                     added = true;
                 }
             }

             if(added) {
                 fetchAlbumList(state, offset + static_cast<int>(albums.size()));
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
    addRomanization(params, state->romanization);
    state->albumIds.pop_front();

    call(state->account, u"getAlbum"_s, params, [this, state](std::optional<QJsonObject> response, ApiError error) {
        if(!response) {
            qCWarning(SUBSONIC) << "Failed to fetch album:" << error.message;
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

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

#pragma once

#include "subsonicapi.h"

#include <core/accountstore.h>
#include <core/library/libraryprovider.h>

#include <QObject>

#include <map>
#include <mutex>

namespace Fooyin {
class NetworkAccessManager;

namespace Subsonic {
struct SyncState;

/*!
 * Provides libraries backed by Subsonic servers. A library path has the form `subsonic://user@host:port/path`; its
 * account (server URL, user name and password) is kept in the AccountStore.
 */
class SubsonicProvider : public QObject,
                         public LibraryProvider
{
    Q_OBJECT

public:
    SubsonicProvider(std::shared_ptr<NetworkAccessManager> network, DbConnectionPoolPtr dbPool,
                     QObject* parent = nullptr);

    [[nodiscard]] QString scheme() const override;
    void fetchTracks(const LibraryInfo& library, FetchCallback callback) override;
    void searchTracks(const LibraryInfo& library, const QString& query, FetchCallback callback) override;
    [[nodiscard]] RemoteStreamSource createStreamSource(const QUrl& url) const override;
    [[nodiscard]] QUrl resolveUrl(const QUrl& url) const override;
    [[nodiscard]] bool reportsPlays(const Track& track) const override;

    [[nodiscard]] static QString libraryPath(const Account& account);
    [[nodiscard]] bool isProviderPath(const QString& path) const;

    //! Loads the stored account of the library at @p libraryPath.
    void loadAccount(const QString& libraryPath);
    bool saveAccount(const QString& libraryPath, const Account& account);
    void removeAccount(const QString& libraryPath);

    /*!
     * Asks the server of the library at @p libraryPath, using @p account, whether it scrobbles plays itself. Calls
     * @p done when the answer has arrived; a failed request keeps what was known.
     */
    void fetchServerInfo(const QString& libraryPath, const Account& account, std::function<void()> done = {});
    //! Returns whether the server of @p libraryPath scrobbles the user's plays itself, if it said so.
    [[nodiscard]] std::optional<bool> serverScrobbles(const QString& libraryPath) const;

    //! Calls @p method on the server owning @p path, a library, track or artwork path.
    void call(const QString& path, const QString& method, const QUrlQuery& params, ResponseCallback callback);
    //! Calls @p method on the server of @p account.
    void call(const Account& account, const QString& method, const QUrlQuery& params, ResponseCallback callback);

    //! Returns the account of the server owning @p path, a library, track or artwork path.
    [[nodiscard]] std::optional<Account> accountForPath(const QString& path) const;

Q_SIGNALS:
    //! Emitted when the complete track list of the library at @p libraryPath has been fetched.
    void tracksFetched(const QString& libraryPath);
    //! Emitted when fetchServerInfo() has updated what is known about the server of @p libraryPath.
    void serverInfoChanged(const QString& libraryPath);

private:
    //! Returns the library path owning @p path, a library, track or artwork path, or an empty string.
    [[nodiscard]] QString libraryPathFor(const QString& path) const;
    void finishSync(const std::shared_ptr<SyncState>& state);

    void fetchSongs(const std::shared_ptr<SyncState>& state, int offset);
    void fetchAlbumList(const std::shared_ptr<SyncState>& state, int offset);
    void fetchNextAlbum(const std::shared_ptr<SyncState>& state);

    std::shared_ptr<NetworkAccessManager> m_network;
    AccountStore m_accountStore;

    mutable std::mutex m_mutex;
    std::map<QString, Account> m_accounts;
    //! By library path, getUser's `scrobblingEnabled` where the server sent it.
    std::map<QString, bool> m_serverScrobbles;
};
} // namespace Subsonic
} // namespace Fooyin

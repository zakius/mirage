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

#include <utils/id.h>

#include <QObject>
#include <QTimer>

#include <map>
#include <set>

namespace Fooyin {
class LibraryManager;
class MusicLibrary;
class Playlist;
class PlaylistHandler;

namespace Subsonic {
class SubsonicProvider;

/*!
 * Mirrors the playlists of Subsonic servers as fooyin playlists, and sends local edits, new playlists and removals
 * back to the server (see "Playlists" in docs/mirage.md).
 */
class PlaylistMirror : public QObject
{
    Q_OBJECT

public:
    PlaylistMirror(SubsonicProvider* provider, LibraryManager* libraryManager, MusicLibrary* library,
                   PlaylistHandler* playlistHandler, QObject* parent = nullptr);

    //! Applies the server's playlists to the mirrored playlists of the library at @p libraryPath.
    void syncPlaylists(const QString& libraryPath);
    //! Sets how long after the last edit the edits are sent, one second by default.
    void setPushDelay(std::chrono::milliseconds delay);

private:
    void playlistEdited(Playlist* playlist);
    void playlistAdded(Playlist* playlist);
    void playlistRemoved(const Playlist* playlist);
    void serverRemoved(const QString& libraryPath);
    void pushPlaylists();
    //! Replaces the entries of the server playlist @p key with @p songIds.
    void sendEntries(const QString& key, const QStringList& songIds, ResponseCallback done);
    void deletePlaylist(const QString& key);
    [[nodiscard]] std::optional<QString> defaultServer() const;
    void uploadPlaylist(Playlist* playlist);
    void linkPlaylist(const UId& id, const QString& server, const QString& serverId);

    SubsonicProvider* m_provider;
    LibraryManager* m_libraryManager;
    MusicLibrary* m_library;
    PlaylistHandler* m_playlistHandler;

    std::set<QString> m_pendingPlaylistSyncs;
    //! Mirrored playlists edited locally and not yet saved on the server, with a count of their edits.
    std::map<QString, int> m_dirtyPlaylists;
    //! Mirrored playlists being sent to the server.
    std::set<QString> m_pushingPlaylists;
    //! Mirrored playlists removed locally and not yet deleted on the server.
    std::set<QString> m_pendingDeletes;
    QTimer m_pushTimer;
    //! Playlists being created on the server.
    std::set<UId> m_uploadingPlaylists;
    //! Counts links of new playlists; a sync started before a playlist was linked does not know it.
    int m_links{0};
    //! The value of m_links when each playlist was linked.
    std::map<QString, int> m_linkedAt;
    bool m_applyingServerChanges{false};
};
} // namespace Subsonic
} // namespace Fooyin

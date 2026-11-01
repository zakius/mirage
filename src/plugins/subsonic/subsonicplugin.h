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

#include "subsonicuploaddialog.h"

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <core/track.h>
#include <gui/plugins/guiplugin.h>
#include <utils/id.h>

#include <QTimer>

#include <map>
#include <set>

class QAction;

namespace Fooyin {
class AudioLoader;
class NetworkAccessManager;
class Playlist;
}

namespace Fooyin::Subsonic {
class SubsonicProvider;

class SubsonicPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "subsonic.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;

    //! Uploads local files to the server of @p libraryPath, or to one the user picks if it is empty.
    void showUpload(const QString& libraryPath = {});
    //! Rips an audio CD to the server of @p libraryPath, or to one the user picks if it is empty.
    void ripCd(const QString& libraryPath = {});

private:
    //! Returns the library path of the server of all @p tracks, or an empty string if they are not on one server.
    [[nodiscard]] QString selectionServer(const TrackList& tracks) const;
    [[nodiscard]] std::vector<UploadDialog::Server> uploadServers() const;
    void updateUploadActions();
    UploadDialog* createUploadDialog(const QString& libraryPath);
    void scrobble(const Track& track, bool submission);
    void writeStats(const TrackList& tracks, Track::Stats stats);
    void syncPlaylists(const QString& libraryPath);
    void playlistEdited(Playlist* playlist);
    void playlistRemoved(const Playlist* playlist);
    void pushPlaylists();
    void deletePlaylist(const QString& key);
    [[nodiscard]] std::optional<QString> defaultServer() const;
    void uploadPlaylist(Playlist* playlist);
    void linkPlaylist(const UId& id, const QString& server, const QString& serverId);

    LibraryManager* m_libraryManager{nullptr};
    MusicLibrary* m_library{nullptr};
    PlaylistHandler* m_playlistHandler{nullptr};
    SettingsManager* m_settings{nullptr};
    std::shared_ptr<AudioLoader> m_audioLoader;
    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<SubsonicProvider> m_provider;
    QAction* m_uploadAction{nullptr};
    QAction* m_ripAction{nullptr};
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
    bool m_applyingServerChanges{false};
};
} // namespace Fooyin::Subsonic

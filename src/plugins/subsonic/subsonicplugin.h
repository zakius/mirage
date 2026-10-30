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

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <core/track.h>
#include <gui/plugins/guiplugin.h>

#include <set>

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

private:
    void scrobble(const Track& track, bool submission);
    void writeStats(const TrackList& tracks, Track::Stats stats);
    void syncPlaylists(const QString& libraryPath);

    LibraryManager* m_libraryManager{nullptr};
    MusicLibrary* m_library{nullptr};
    PlaylistHandler* m_playlistHandler{nullptr};
    SettingsManager* m_settings{nullptr};
    std::shared_ptr<SubsonicProvider> m_provider;
    std::set<QString> m_pendingPlaylistSyncs;
};
} // namespace Fooyin::Subsonic

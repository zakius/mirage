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

class QAction;
class QMenu;

namespace Fooyin {
class AudioLoader;
class NetworkAccessManager;
}

namespace Fooyin::Subsonic {
class NameDisplayPage;
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
    //! Shows the upload and name display actions only where a server offers them.
    void updateServerActions();
    UploadDialog* createUploadDialog(const QString& libraryPath);
    void scrobble(const Track& track, bool submission);
    void writeStats(const TrackList& tracks, Track::Stats stats);

    LibraryManager* m_libraryManager{nullptr};
    MusicLibrary* m_library{nullptr};
    PlaylistHandler* m_playlistHandler{nullptr};
    SettingsManager* m_settings{nullptr};
    std::shared_ptr<AudioLoader> m_audioLoader;
    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<SubsonicProvider> m_provider;
    QAction* m_uploadAction{nullptr};
    QAction* m_ripAction{nullptr};
    NameDisplayPage* m_nameDisplayPage{nullptr};
    //! View > Names and Lyrics, owned by the View menu.
    QMenu* m_nameDisplayMenu{nullptr};
};
} // namespace Fooyin::Subsonic

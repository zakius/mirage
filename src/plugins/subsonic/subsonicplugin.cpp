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

#include "subsonicplugin.h"

#include "subsonicpage.h"
#include "subsonicprovider.h"

#include <core/coresettings.h>
#include <core/library/librarymanager.h>
#include <core/library/libraryprovider.h>
#include <core/library/musiclibrary.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <utils/settings/settingsmanager.h>

#include <QDateTime>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QScopedValueRollback>

Q_LOGGING_CATEGORY(SUBSONIC_PLUGIN, "fy.subsonic.plugin")

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

constexpr auto PlaylistProperty = "SubsonicPlaylist";

namespace {
QString playlistKey(const Fooyin::Playlist* playlist)
{
    return playlist->extraProperties().value(QString::fromLatin1(PlaylistProperty));
}

void logError(const QString& method, const std::optional<QJsonObject>& response,
              const Fooyin::Subsonic::ApiError& error)
{
    if(!response) {
        qCWarning(SUBSONIC_PLUGIN) << method << "failed:" << error.message;
    }
}
} // namespace

namespace Fooyin::Subsonic {
void SubsonicPlugin::initialise(const CorePluginContext& context)
{
    m_libraryManager  = context.libraryManager;
    m_library         = context.library;
    m_playlistHandler = context.playlistHandler;
    m_settings        = context.settingsManager;

    m_provider = std::make_shared<SubsonicProvider>(context.networkAccess, context.dbPool);
    context.libraryProviders->addProvider(m_provider);

    for(const auto& [id, library] : m_libraryManager->allLibraries()) {
        if(context.libraryProviders->providerForPath(library.path) == m_provider) {
            m_provider->loadAccount(library.path);
        }
    }

    // Server libraries change without local file changes, so they are synced on every start
    QObject::connect(m_library, &MusicLibrary::tracksLoaded, this, [this]() {
        if(m_settings->value<Settings::Core::AutoRefresh>()) {
            return;
        }
        for(const auto& [id, library] : m_libraryManager->allLibraries()) {
            if(m_provider->isProviderPath(library.path)) {
                m_library->refresh(library);
            }
        }
    });
    QObject::connect(m_libraryManager, &LibraryManager::libraryRemoved, this, [this](const LibraryInfo& library) {
        if(m_provider->isProviderPath(library.path)) {
            m_provider->removeAccount(library.path);
        }
    });

    QObject::connect(context.playerController, &PlayerController::currentTrackChanged, this,
                     [this](const Track& track) { scrobble(track, false); });
    QObject::connect(context.playerController, &PlayerController::trackPlayed, this,
                     [this](const Track& track) { scrobble(track, true); });
    QObject::connect(m_library, &MusicLibrary::tracksStatsChanged, this, &SubsonicPlugin::writeStats);

    // Playlists refer to library tracks, so they are synced once the fetched tracks have been applied
    QObject::connect(m_provider.get(), &SubsonicProvider::tracksFetched, this,
                     [this](const QString& libraryPath) { m_pendingPlaylistSyncs.emplace(libraryPath); });
    // Local edits of mirrored playlists are sent back to the server
    const auto markEdited = [this](Playlist* playlist) {
        playlistEdited(playlist);
    };
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksAdded, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksPatched, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksChanged, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksRemoved, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistRenamed, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistRemoved, this, &SubsonicPlugin::playlistRemoved);
    // New playlists are created on the server
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistAdded, this, &SubsonicPlugin::uploadPlaylist);
    m_pushTimer.setSingleShot(true);
    m_pushTimer.setInterval(1s);
    QObject::connect(&m_pushTimer, &QTimer::timeout, this, &SubsonicPlugin::pushPlaylists);

    QObject::connect(m_library, &MusicLibrary::scanFinished, this,
                     [this](int /*id*/, ScanRequest::Type type, bool cancelled) {
                         if(type != ScanRequest::Library || cancelled) {
                             return;
                         }
                         for(const QString& libraryPath : std::exchange(m_pendingPlaylistSyncs, {})) {
                             syncPlaylists(libraryPath);
                         }
                         // Playlists made before a server was added, or whose upload failed
                         for(Playlist* playlist : m_playlistHandler->playlists()) {
                             uploadPlaylist(playlist);
                         }
                     });
}

void SubsonicPlugin::initialise(const GuiPluginContext& /*context*/)
{
    new SubsonicPage(m_provider.get(), m_libraryManager, m_library, m_settings, this);
}

void SubsonicPlugin::scrobble(const Track& track, bool submission)
{
    if(!m_provider->isProviderPath(track.filepath())) {
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"id"_s, songId(track.filepath()));
    params.addQueryItem(u"submission"_s, submission ? u"true"_s : u"false"_s);
    if(submission) {
        params.addQueryItem(u"time"_s, QString::number(QDateTime::currentMSecsSinceEpoch()));
    }

    m_provider->call(track.filepath(), u"scrobble"_s, params, [](std::optional<QJsonObject> response, ApiError error) {
        logError(u"scrobble"_s, response, error);
    });
}

void SubsonicPlugin::writeStats(const TrackList& tracks, Track::Stats stats)
{
    for(const Track& track : tracks) {
        if(!m_provider->isProviderPath(track.filepath())) {
            continue;
        }

        QUrlQuery params;
        params.addQueryItem(u"id"_s, songId(track.filepath()));

        if(stats.testFlag(Track::Stat::Rating)) {
            // Subsonic ratings are whole stars, fooyin's are half stars
            QUrlQuery ratingParams{params};
            ratingParams.addQueryItem(u"rating"_s, QString::number((track.ratingStars() + 1) / 2));
            m_provider->call(
                track.filepath(), u"setRating"_s, ratingParams,
                [](std::optional<QJsonObject> response, ApiError error) { logError(u"setRating"_s, response, error); });
        }
        if(stats.testFlag(Track::Stat::Loved)) {
            const QString method = track.isLoved() ? u"star"_s : u"unstar"_s;
            m_provider->call(
                track.filepath(), method, params,
                [method](std::optional<QJsonObject> response, ApiError error) { logError(method, response, error); });
        }
    }
}

void SubsonicPlugin::playlistEdited(const Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist) {
        return;
    }
    if(const QString key = playlistKey(playlist); m_provider->isProviderPath(key)) {
        m_dirtyPlaylists.emplace(key);
        m_pushTimer.start();
    }
}

void SubsonicPlugin::playlistRemoved(const Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist) {
        return;
    }

    const QString key = playlistKey(playlist);
    if(!m_provider->isProviderPath(key)) {
        return;
    }
    m_dirtyPlaylists.erase(key);

    QUrlQuery params;
    params.addQueryItem(u"id"_s, key.sliced(key.lastIndexOf('/'_L1) + 1));
    m_provider->call(key, u"deletePlaylist"_s, params, [](std::optional<QJsonObject> response, ApiError error) {
        logError(u"deletePlaylist"_s, response, error);
    });
}

std::optional<QString> SubsonicPlugin::defaultServer() const
{
    for(const auto& [id, library] : m_libraryManager->allLibraries()) {
        if(m_provider->isProviderPath(library.path)) {
            return library.path;
        }
    }
    return {};
}

void SubsonicPlugin::uploadPlaylist(Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist || playlist->isTemporary() || playlist->isAutoPlaylist()
       || !playlistKey(playlist).isEmpty() || m_uploadingPlaylists.contains(playlist->id())) {
        return;
    }

    const auto server = defaultServer();
    if(!server) {
        return;
    }

    const QString songsPrefix = *server + u"/song/"_s;
    QUrlQuery params;
    params.addQueryItem(u"name"_s, playlist->name());
    for(const Track& track : playlist->tracks()) {
        if(track.filepath().startsWith(songsPrefix)) {
            params.addQueryItem(u"songId"_s, songId(track.filepath()));
        }
    }

    m_uploadingPlaylists.emplace(playlist->id());
    m_provider->call(
        *server, u"createPlaylist"_s, params,
        [this, id = playlist->id(), name = playlist->name(), server = *server](std::optional<QJsonObject> response,
                                                                               ApiError error) {
            if(!response) {
                m_uploadingPlaylists.erase(id);
                logError(u"createPlaylist"_s, response, error);
                return;
            }

            const QString serverId = response->value("playlist"_L1).toObject().value("id"_L1).toVariant().toString();
            if(!serverId.isEmpty()) {
                linkPlaylist(id, server, serverId);
                return;
            }

            // Servers before API 1.14 do not return the new playlist, so find it by name
            m_provider->call(
                server, u"getPlaylists"_s, {},
                [this, id, name, server](std::optional<QJsonObject> playlists, ApiError) {
                    QString newest;
                    for(const auto& value : toArray(
                            playlists.value_or(QJsonObject{}).value("playlists"_L1).toObject().value("playlist"_L1))) {
                        const QJsonObject candidate = value.toObject();
                        if(candidate.value("name"_L1).toString() == name) {
                            newest = candidate.value("id"_L1).toVariant().toString();
                        }
                    }
                    linkPlaylist(id, server, newest);
                });
        });
}

void SubsonicPlugin::linkPlaylist(const UId& id, const QString& server, const QString& serverId)
{
    m_uploadingPlaylists.erase(id);

    auto* playlist = m_playlistHandler->playlistById(id);
    if(!playlist || serverId.isEmpty()) {
        return;
    }

    const QString key = server + u"/playlist/"_s + serverId;
    playlist->setExtraProperty(QString::fromLatin1(PlaylistProperty), key);

    // Send edits made while the playlist was being created
    m_dirtyPlaylists.emplace(key);
    m_pushTimer.start();
}

void SubsonicPlugin::pushPlaylists()
{
    for(const QString& key : std::exchange(m_dirtyPlaylists, {})) {
        const auto playlists = m_playlistHandler->playlists();
        const auto it = std::ranges::find_if(playlists,
                                             [&key](const Playlist* playlist) { return playlistKey(playlist) == key; });
        if(it == playlists.cend()) {
            continue;
        }

        const QString serverId    = key.sliced(key.lastIndexOf('/'_L1) + 1);
        const QString songsPrefix = key.left(key.lastIndexOf(u"/playlist/"_s)) + u"/song/"_s;

        // createPlaylist with a playlist id replaces its entries; tracks from elsewhere cannot be added
        QUrlQuery entries;
        entries.addQueryItem(u"playlistId"_s, serverId);
        for(const Track& track : (*it)->tracks()) {
            if(track.filepath().startsWith(songsPrefix)) {
                entries.addQueryItem(u"songId"_s, songId(track.filepath()));
            }
        }
        m_provider->call(key, u"createPlaylist"_s, entries, [](std::optional<QJsonObject> response, ApiError error) {
            logError(u"createPlaylist"_s, response, error);
        });

        QUrlQuery rename;
        rename.addQueryItem(u"playlistId"_s, serverId);
        rename.addQueryItem(u"name"_s, (*it)->name());
        m_provider->call(key, u"updatePlaylist"_s, rename, [](std::optional<QJsonObject> response, ApiError error) {
            logError(u"updatePlaylist"_s, response, error);
        });
    }
}

void SubsonicPlugin::syncPlaylists(const QString& libraryPath)
{
    m_provider->call(
        libraryPath, u"getPlaylists"_s, {}, [this, libraryPath](std::optional<QJsonObject> response, ApiError error) {
            if(!response) {
                logError(u"getPlaylists"_s, response, error);
                return;
            }

            const QString keyPrefix = libraryPath + u"/playlist/"_s;
            std::set<QString> serverKeys;

            for(const auto& value : toArray(response->value("playlists"_L1).toObject().value("playlist"_L1))) {
                const QString id = value.toObject().value("id"_L1).toString();
                serverKeys.emplace(keyPrefix + id);

                QUrlQuery params;
                params.addQueryItem(u"id"_s, id);
                m_provider->call(libraryPath, u"getPlaylist"_s, params,
                                 [this, libraryPath, key = QString{keyPrefix + id}](
                                     std::optional<QJsonObject> playlistResponse, ApiError playlistError) {
                                     if(!playlistResponse) {
                                         logError(u"getPlaylist"_s, playlistResponse, playlistError);
                                         return;
                                     }

                                     const QJsonObject playlist = playlistResponse->value("playlist"_L1).toObject();
                                     const QString name         = playlist.value("name"_L1).toString();

                                     std::unordered_map<QString, Track> tracksByPath;
                                     for(const Track& track : m_library->libraryTracks()) {
                                         tracksByPath.emplace(track.filepath(), track);
                                     }

                                     TrackList tracks;
                                     for(const auto& entry : toArray(playlist.value("entry"_L1))) {
                                         const QString path = songToTrack(entry.toObject(), libraryPath).filepath();
                                         if(const auto it = tracksByPath.find(path); it != tracksByPath.cend()) {
                                             tracks.push_back(it->second);
                                         }
                                     }

                                     const auto playlists = m_playlistHandler->playlists();
                                     const auto existing
                                         = std::ranges::find_if(playlists, [&key](const Playlist* candidate) {
                                               return playlistKey(candidate) == key;
                                           });

                                     // Local edits waiting to be sent win over the server state
                                     if(m_dirtyPlaylists.contains(key)) {
                                         return;
                                     }

                                     const QScopedValueRollback applying{m_applyingServerChanges, true};

                                     if(existing == playlists.cend()) {
                                         if(auto* created = m_playlistHandler->createNewPlaylist(name, tracks)) {
                                             created->setExtraProperty(QString::fromLatin1(PlaylistProperty), key);
                                         }
                                         return;
                                     }

                                     if((*existing)->name() != name) {
                                         m_playlistHandler->renamePlaylist((*existing)->id(), name);
                                     }
                                     if((*existing)->tracks() != tracks) {
                                         m_playlistHandler->replacePlaylistTracks((*existing)->id(), tracks);
                                     }
                                 });
            }

            // Server playlists are mirrored, so playlists deleted on the server are removed
            const QScopedValueRollback applying{m_applyingServerChanges, true};
            for(const Playlist* playlist : m_playlistHandler->playlists()) {
                const QString key = playlistKey(playlist);
                if(key.startsWith(keyPrefix) && !serverKeys.contains(key)) {
                    m_playlistHandler->removePlaylist(playlist->id());
                }
            }
        });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicplugin.cpp"

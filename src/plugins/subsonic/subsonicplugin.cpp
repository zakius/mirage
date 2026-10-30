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

Q_LOGGING_CATEGORY(SUBSONIC_PLUGIN, "fy.subsonic.plugin")

using namespace Qt::StringLiterals;

constexpr auto PlaylistProperty = "SubsonicPlaylist";

namespace {
void logError(const QString& method, const std::optional<QJsonObject>& response, const QString& error)
{
    if(!response) {
        qCWarning(SUBSONIC_PLUGIN) << method << "failed:" << error;
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
    QObject::connect(m_library, &MusicLibrary::scanFinished, this,
                     [this](int /*id*/, ScanRequest::Type type, bool cancelled) {
                         if(type != ScanRequest::Library || cancelled) {
                             return;
                         }
                         for(const QString& libraryPath : std::exchange(m_pendingPlaylistSyncs, {})) {
                             syncPlaylists(libraryPath);
                         }
                     });
}

void SubsonicPlugin::initialise(const GuiPluginContext& /*context*/)
{
    new SubsonicPage(m_provider.get(), m_libraryManager, m_settings, this);
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

    m_provider->call(track.filepath(), u"scrobble"_s, params, [](std::optional<QJsonObject> response, QString error) {
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
                [](std::optional<QJsonObject> response, QString error) { logError(u"setRating"_s, response, error); });
        }
        if(stats.testFlag(Track::Stat::Loved)) {
            const QString method = track.isLoved() ? u"star"_s : u"unstar"_s;
            m_provider->call(
                track.filepath(), method, params,
                [method](std::optional<QJsonObject> response, QString error) { logError(method, response, error); });
        }
    }
}

void SubsonicPlugin::syncPlaylists(const QString& libraryPath)
{
    m_provider->call(
        libraryPath, u"getPlaylists"_s, {}, [this, libraryPath](std::optional<QJsonObject> response, QString error) {
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
                m_provider->call(
                    libraryPath, u"getPlaylist"_s, params,
                    [this, libraryPath, key = QString{keyPrefix + id}](std::optional<QJsonObject> playlistResponse,
                                                                       QString playlistError) {
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
                        const auto existing  = std::ranges::find_if(playlists, [&key](const Playlist* candidate) {
                            return candidate->extraProperties().value(QString::fromLatin1(PlaylistProperty)) == key;
                        });

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
            for(const Playlist* playlist : m_playlistHandler->playlists()) {
                const QString key = playlist->extraProperties().value(QString::fromLatin1(PlaylistProperty));
                if(key.startsWith(keyPrefix) && !serverKeys.contains(key)) {
                    m_playlistHandler->removePlaylist(playlist->id());
                }
            }
        });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicplugin.cpp"

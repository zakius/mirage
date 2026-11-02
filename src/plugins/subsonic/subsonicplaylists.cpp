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

#include "subsonicplaylists.h"

#include "subsonicprovider.h"

#include <core/library/librarymanager.h>
#include <core/library/musiclibrary.h>
#include <core/playlist/playlisthandler.h>

#include <QJsonArray>
#include <QLoggingCategory>
#include <QScopedValueRollback>

Q_LOGGING_CATEGORY(SUBSONIC_PLAYLISTS, "fy.subsonic.playlists")

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

constexpr auto PlaylistProperty = "SubsonicPlaylist";
//! Set while a mirrored playlist has edits the server has not saved, so they survive a restart.
constexpr auto UnsavedProperty = "SubsonicUnsaved";
//! Set on mirrored playlists the server does not let this user change, such as another user's.
constexpr auto ReadOnlyProperty = "SubsonicReadOnly";
constexpr int NotAuthorizedCode = 50;
constexpr int NotFoundCode      = 70;
//! Song ids per request where they go in the URL, so that servers and proxies accept its length.
constexpr qsizetype SongIdBatch = 100;

namespace {
QString playlistKey(const Fooyin::Playlist* playlist)
{
    return playlist->extraProperties().value(QString::fromLatin1(PlaylistProperty));
}

bool isReadOnly(const Fooyin::Playlist* playlist)
{
    return playlist->hasExtraProperty(QString::fromLatin1(ReadOnlyProperty));
}

//! True if the server refuses a change for good, so retrying cannot help.
bool isRefused(const Fooyin::Subsonic::ApiError& error)
{
    return error.code == NotAuthorizedCode || error.code == NotFoundCode;
}

QString serverOf(const QString& playlistKey)
{
    return playlistKey.left(playlistKey.lastIndexOf(u"/playlist/"_s));
}

QString serverIdOf(const QString& playlistKey)
{
    return playlistKey.sliced(playlistKey.lastIndexOf('/'_L1) + 1);
}

//! Returns the song ids of the entries of @p playlist that are songs of the server at @p libraryPath.
QStringList songIds(const Fooyin::Playlist* playlist, const QString& libraryPath)
{
    const QString songsPrefix = libraryPath + u"/song/"_s;
    QStringList ids;
    for(const Fooyin::Track& track : playlist->tracks()) {
        if(track.filepath().startsWith(songsPrefix)) {
            ids.append(Fooyin::Subsonic::songId(track.filepath()));
        }
    }
    return ids;
}

void logError(const QString& method, const std::optional<QJsonObject>& response,
              const Fooyin::Subsonic::ApiError& error)
{
    if(!response) {
        qCWarning(SUBSONIC_PLAYLISTS) << method << "failed:" << error.message;
    }
}

//! Adds @p songIds to the server playlist @p key in batches, after the answer @p response to the request before.
void addInBatches(Fooyin::Subsonic::SubsonicProvider* provider, const QString& key, const QStringList& songIds,
                  std::optional<QJsonObject> response, const Fooyin::Subsonic::ApiError& error,
                  const Fooyin::Subsonic::ResponseCallback& done)
{
    if(!response || songIds.isEmpty()) {
        done(std::move(response), error);
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"playlistId"_s, serverIdOf(key));
    for(const QString& id : songIds.first(std::min(SongIdBatch, songIds.size()))) {
        params.addQueryItem(u"songIdToAdd"_s, id);
    }
    provider->call(key, u"updatePlaylist"_s, params,
                   [provider, key, rest = songIds.mid(SongIdBatch), done](std::optional<QJsonObject> next,
                                                                          const Fooyin::Subsonic::ApiError& nextError) {
                       addInBatches(provider, key, rest, std::move(next), nextError, done);
                   });
}
} // namespace

namespace Fooyin::Subsonic {
PlaylistMirror::PlaylistMirror(SubsonicProvider* provider, LibraryManager* libraryManager, MusicLibrary* library,
                               PlaylistHandler* playlistHandler, QObject* parent)
    : QObject{parent}
    , m_provider{provider}
    , m_libraryManager{libraryManager}
    , m_library{library}
    , m_playlistHandler{playlistHandler}
{
    // Playlists refer to library tracks, so they are synced once the fetched tracks have been applied
    QObject::connect(m_provider, &SubsonicProvider::tracksFetched, this,
                     [this](const QString& libraryPath) { m_pendingPlaylistSyncs.emplace(libraryPath); });
    QObject::connect(m_libraryManager, &LibraryManager::libraryAboutToBeRemoved, this,
                     [this](const LibraryInfo& removed) { serverRemoved(removed.path); });

    // Local edits of mirrored playlists are sent back to the server. Entries removed because their tracks left the
    // library (tracksRemoved, for example when a server library is removed) are not edits.
    const auto markEdited = [this](Playlist* playlist) {
        playlistEdited(playlist);
    };
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksAdded, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksPatched, this, markEdited);
    // Changes with indexes only refresh the metadata of entries, for example after a sync
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksChanged, this,
                     [this](Playlist* playlist, const std::vector<int>& indexes) {
                         if(indexes.empty()) {
                             playlistEdited(playlist);
                         }
                     });
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistRenamed, this, markEdited);
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistRemoved, this, &PlaylistMirror::playlistRemoved);
    // New playlists are created on the server
    QObject::connect(m_playlistHandler, &PlaylistHandler::playlistAdded, this, &PlaylistMirror::playlistAdded);
    m_pushTimer.setSingleShot(true);
    m_pushTimer.setInterval(1s);
    QObject::connect(&m_pushTimer, &QTimer::timeout, this, &PlaylistMirror::pushPlaylists);

    QObject::connect(m_library, &MusicLibrary::scanFinished, this,
                     [this](int /*id*/, ScanRequest::Type type, bool cancelled) {
                         if(type != ScanRequest::Library || cancelled) {
                             return;
                         }
                         for(const Playlist* playlist : m_playlistHandler->playlists()) {
                             if(playlist->hasExtraProperty(QString::fromLatin1(UnsavedProperty))) {
                                 m_dirtyPlaylists.try_emplace(playlistKey(playlist), 0);
                             }
                         }
                         for(const QString& libraryPath : std::exchange(m_pendingPlaylistSyncs, {})) {
                             syncPlaylists(libraryPath);
                         }
                         // Playlists made before a server was added, or whose upload failed
                         for(Playlist* playlist : m_playlistHandler->playlists()) {
                             uploadPlaylist(playlist);
                         }
                         // Edits whose push failed
                         if(!m_dirtyPlaylists.empty()) {
                             m_pushTimer.start();
                         }
                     });
}

void PlaylistMirror::setPushDelay(std::chrono::milliseconds delay)
{
    m_pushTimer.setInterval(delay);
}

void PlaylistMirror::playlistEdited(Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist || isReadOnly(playlist)) {
        return;
    }
    const QString key = playlistKey(playlist);
    if(key.isEmpty()) {
        // For example fooyin's empty "Default" playlist, once it gets tracks
        uploadPlaylist(playlist);
    }
    else if(m_provider->isProviderPath(key)) {
        ++m_dirtyPlaylists[key];
        playlist->setExtraProperty(QString::fromLatin1(UnsavedProperty), u"1"_s);
        m_pushTimer.start();
    }
}

void PlaylistMirror::playlistAdded(Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist) {
        return;
    }

    // A removed playlist that comes back (undo) was deleted on the server, so it is created there again
    const QString key = playlistKey(playlist);
    if(!key.isEmpty() && m_provider->isProviderPath(key) && !isReadOnly(playlist)) {
        playlist->removeExtraProperty(QString::fromLatin1(PlaylistProperty));
        playlist->removeExtraProperty(QString::fromLatin1(UnsavedProperty));
    }
    uploadPlaylist(playlist);
}

void PlaylistMirror::playlistRemoved(const Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist) {
        return;
    }

    const QString key = playlistKey(playlist);
    // The server keeps read-only playlists, so they come back on the next sync
    if(!m_provider->isProviderPath(key) || isReadOnly(playlist)) {
        return;
    }
    m_dirtyPlaylists.erase(key);
    m_pendingDeletes.emplace(key);
    deletePlaylist(key);
}

void PlaylistMirror::serverRemoved(const QString& libraryPath)
{
    // Its playlists stay as local copies; edits not yet sent are dropped, so adding the server again shows its state
    const QString keyPrefix = libraryPath + u"/playlist/"_s;
    const auto ofServer     = [&keyPrefix](const auto& key) {
        return key.startsWith(keyPrefix);
    };
    std::erase_if(m_dirtyPlaylists, [&ofServer](const auto& entry) { return ofServer(entry.first); });
    std::erase_if(m_pendingDeletes, ofServer);
    m_pendingPlaylistSyncs.erase(libraryPath);
    for(Playlist* playlist : m_playlistHandler->playlists()) {
        if(ofServer(playlistKey(playlist))) {
            playlist->removeExtraProperty(QString::fromLatin1(UnsavedProperty));
        }
    }
}

void PlaylistMirror::deletePlaylist(const QString& key)
{
    QUrlQuery params;
    params.addQueryItem(u"id"_s, serverIdOf(key));
    m_provider->call(key, u"deletePlaylist"_s, params,
                     [this, key](std::optional<QJsonObject> response, ApiError error) {
                         // Failed deletes are retried on the next sync
                         if(response || isRefused(error)) {
                             m_pendingDeletes.erase(key);
                         }
                         logError(u"deletePlaylist"_s, response, error);
                     });
}

std::optional<QString> PlaylistMirror::defaultServer() const
{
    for(const auto& [id, library] : m_libraryManager->allLibraries()) {
        if(m_provider->isProviderPath(library.path)) {
            return library.path;
        }
    }
    return {};
}

void PlaylistMirror::uploadPlaylist(Playlist* playlist)
{
    if(m_applyingServerChanges || !playlist || playlist->isTemporary() || playlist->isAutoPlaylist()
       || !playlistKey(playlist).isEmpty() || m_uploadingPlaylists.contains(playlist->id())) {
        return;
    }
    // fooyin creates an empty "Default" playlist whenever there is none; it is uploaded once it gets tracks
    if(playlist->name() == "Default"_L1 && playlist->trackCount() == 0) {
        return;
    }

    const auto server = defaultServer();
    if(!server) {
        return;
    }

    // Without formPost only the first ids go along; the push after linking sends them all
    const bool formPost = m_provider->hasFormPost(*server);
    const QStringList ids = songIds(playlist, *server);
    QUrlQuery params;
    params.addQueryItem(u"name"_s, playlist->name());
    for(const QString& id : formPost ? ids : ids.first(std::min(SongIdBatch, ids.size()))) {
        params.addQueryItem(u"songId"_s, id);
    }

    m_uploadingPlaylists.emplace(playlist->id());
    m_provider->callForm(
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

void PlaylistMirror::linkPlaylist(const UId& id, const QString& server, const QString& serverId)
{
    m_uploadingPlaylists.erase(id);

    auto* playlist = m_playlistHandler->playlistById(id);
    if(!playlist || serverId.isEmpty()) {
        return;
    }

    const QString key = server + u"/playlist/"_s + serverId;
    playlist->setExtraProperty(QString::fromLatin1(PlaylistProperty), key);
    m_linkedAt[key] = ++m_links;

    // Send edits made while the playlist was being created
    ++m_dirtyPlaylists[key];
    playlist->setExtraProperty(QString::fromLatin1(UnsavedProperty), u"1"_s);
    m_pushTimer.start();
}

void PlaylistMirror::sendEntries(const QString& key, const QStringList& songIds, ResponseCallback done)
{
    // createPlaylist with a playlist id replaces its entries
    QUrlQuery params;
    params.addQueryItem(u"playlistId"_s, serverIdOf(key));

    if(m_provider->hasFormPost(key)) {
        for(const QString& id : songIds) {
            params.addQueryItem(u"songId"_s, id);
        }
        m_provider->callForm(key, u"createPlaylist"_s, params, std::move(done));
        return;
    }

    // Without formPost the ids go in the URL, so the rest are added in batches
    for(const QString& id : songIds.first(std::min(SongIdBatch, songIds.size()))) {
        params.addQueryItem(u"songId"_s, id);
    }
    m_provider->call(key, u"createPlaylist"_s, params,
                     [this, key, rest = songIds.mid(SongIdBatch), done = std::move(done)](
                         std::optional<QJsonObject> response, const ApiError& error) {
                         addInBatches(m_provider, key, rest, std::move(response), error, done);
                     });
}

void PlaylistMirror::pushPlaylists()
{
    const auto playlists = m_playlistHandler->playlists();

    for(const auto& [key, edits] : std::map{m_dirtyPlaylists}) {
        // One push per playlist at a time, so the server cannot apply them out of order
        if(m_pushingPlaylists.contains(key)) {
            continue;
        }
        const auto it = std::ranges::find_if(playlists,
                                             [&key](const Playlist* playlist) { return playlistKey(playlist) == key; });
        if(it == playlists.cend()) {
            m_dirtyPlaylists.erase(key);
            continue;
        }

        // Tracks from elsewhere cannot be added
        const QStringList ids = songIds(*it, serverOf(key));
        QUrlQuery rename;
        rename.addQueryItem(u"playlistId"_s, serverIdOf(key));
        rename.addQueryItem(u"name"_s, (*it)->name());

        // An edit stays dirty until the server has saved it, so a failed push is retried and syncs do not undo it
        const auto finish = [this, key, edits](const QString& method, const std::optional<QJsonObject>& response,
                                               const ApiError& error) {
            m_pushingPlaylists.erase(key);
            logError(method, response, error);
            // The server will never take these edits, so its copy replaces them
            const bool refused = !response && isRefused(error);
            if(refused || (response && m_dirtyPlaylists.contains(key) && m_dirtyPlaylists.at(key) == edits)) {
                m_dirtyPlaylists.erase(key);
                for(Playlist* playlist : m_playlistHandler->playlists()) {
                    if(playlistKey(playlist) == key) {
                        playlist->removeExtraProperty(QString::fromLatin1(UnsavedProperty));
                    }
                }
                if(refused) {
                    syncPlaylists(serverOf(key));
                }
            }
            else if(response && m_dirtyPlaylists.contains(key)) {
                m_pushTimer.start();
            }
        };

        m_pushingPlaylists.emplace(key);
        sendEntries(key, ids, [this, key, rename, finish](std::optional<QJsonObject> response, ApiError error) {
            if(!response) {
                finish(u"createPlaylist"_s, response, error);
                return;
            }
            m_provider->call(key, u"updatePlaylist"_s, rename,
                             [finish](std::optional<QJsonObject> renamed, ApiError renameError) {
                                 finish(u"updatePlaylist"_s, renamed, renameError);
                             });
        });
    }
}

void PlaylistMirror::syncPlaylists(const QString& libraryPath)
{
    // Playlists linked after this point are missing from the answer, but exist on the server
    const int links = m_links;

    m_provider->call(
        libraryPath, u"getPlaylists"_s, {},
        [this, libraryPath, links](std::optional<QJsonObject> response, ApiError error) {
            if(!response) {
                logError(u"getPlaylists"_s, response, error);
                return;
            }

            const QString keyPrefix = libraryPath + u"/playlist/"_s;
            std::set<QString> serverKeys;

            for(const auto& value : toArray(response->value("playlists"_L1).toObject().value("playlist"_L1))) {
                const QString id = value.toObject().value("id"_L1).toVariant().toString();
                serverKeys.emplace(keyPrefix + id);

                if(m_pendingDeletes.contains(keyPrefix + id)) {
                    deletePlaylist(keyPrefix + id);
                    continue;
                }

                QUrlQuery params;
                params.addQueryItem(u"id"_s, id);
                if(m_provider->hasRomanization(libraryPath)) {
                    params.addQueryItem(u"romanization"_s, u"both"_s);
                }
                m_provider->call(libraryPath, u"getPlaylist"_s, params,
                                 [this, libraryPath, key = QString{keyPrefix + id}](
                                     std::optional<QJsonObject> playlistResponse, ApiError playlistError) {
                                     if(!playlistResponse) {
                                         logError(u"getPlaylist"_s, playlistResponse, playlistError);
                                         return;
                                     }

                                     const QJsonObject playlist = playlistResponse->value("playlist"_L1).toObject();
                                     const QString name         = playlist.value("name"_L1).toString();
                                     // OpenSubsonic marks read-only playlists; Subsonic only lets owners edit. User
                                     // names compare as the server compares them, without case.
                                     const auto account  = m_provider->accountForPath(libraryPath);
                                     const QString owner = playlist.value("owner"_L1).toString();
                                     const bool readOnly
                                         = playlist.value("readonly"_L1).toBool()
                                        || (account && !owner.isEmpty()
                                            && owner.compare(account->username, Qt::CaseInsensitive) != 0);

                                     std::unordered_map<QString, Track> tracksByPath;
                                     for(const Track& track : m_library->libraryTracks()) {
                                         tracksByPath.emplace(track.filepath(), track);
                                     }

                                     // Songs outside the library, such as radio tracks, are kept as they are
                                     TrackList tracks;
                                     for(const auto& entry : toArray(playlist.value("entry"_L1))) {
                                         Track track = songToTrack(entry.toObject(), libraryPath);
                                         if(const auto it = tracksByPath.find(track.filepath());
                                            it != tracksByPath.cend()) {
                                             tracks.push_back(it->second);
                                         }
                                         else {
                                             track.generateHash();
                                             tracks.push_back(track);
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

                                     const auto setReadOnly = [readOnly](Playlist* target) {
                                         if(readOnly) {
                                             target->setExtraProperty(QString::fromLatin1(ReadOnlyProperty), u"1"_s);
                                         }
                                         else {
                                             target->removeExtraProperty(QString::fromLatin1(ReadOnlyProperty));
                                         }
                                     };

                                     if(existing == playlists.cend()) {
                                         if(auto* created = m_playlistHandler->createNewPlaylist(name, tracks)) {
                                             created->setExtraProperty(QString::fromLatin1(PlaylistProperty), key);
                                             setReadOnly(created);
                                             m_playlistHandler->setPlaylistLocked(created->id(), readOnly);
                                         }
                                         return;
                                     }

                                     // fooyin refuses changes to locked playlists, so the lock is lifted to apply them
                                     const bool locked
                                         = readOnly || ((*existing)->isLocked() && !isReadOnly(*existing));
                                     setReadOnly(*existing);
                                     m_playlistHandler->setPlaylistLocked((*existing)->id(), false);

                                     if((*existing)->name() != name) {
                                         m_playlistHandler->renamePlaylist((*existing)->id(), name);
                                     }
                                     // Entries the server cannot hold, such as local files, keep their place
                                     const QString songsPrefix = libraryPath + u"/song/"_s;
                                     TrackList merged;
                                     auto next = tracks.cbegin();
                                     for(const Track& track : (*existing)->tracks()) {
                                         if(!track.filepath().startsWith(songsPrefix)) {
                                             merged.push_back(track);
                                         }
                                         else if(next != tracks.cend()) {
                                             merged.push_back(*next++);
                                         }
                                     }
                                     merged.insert(merged.end(), next, tracks.cend());

                                     if((*existing)->tracks() != merged) {
                                         m_playlistHandler->replacePlaylistTracks((*existing)->id(), merged);
                                     }
                                     m_playlistHandler->setPlaylistLocked((*existing)->id(), locked);
                                 });
            }

            std::erase_if(m_pendingDeletes, [&keyPrefix, &serverKeys](const QString& key) {
                return key.startsWith(keyPrefix) && !serverKeys.contains(key);
            });

            // Server playlists are mirrored, so playlists deleted on the server are removed
            const QScopedValueRollback applying{m_applyingServerChanges, true};
            for(const Playlist* playlist : m_playlistHandler->playlists()) {
                const QString key = playlistKey(playlist);
                const auto linked = m_linkedAt.find(key);
                if(key.startsWith(keyPrefix) && !serverKeys.contains(key)
                   && (linked == m_linkedAt.cend() || linked->second <= links)) {
                    m_playlistHandler->removePlaylist(playlist->id());
                }
            }
        });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicplaylists.cpp"

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
#include "subsonicuploaddialog.h"

#include "../cdda/cddadiscripper.h"

#include <core/coresettings.h>
#include <core/library/librarymanager.h>
#include <core/library/libraryprovider.h>
#include <core/library/musiclibrary.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <core/scripting/displayname.h>
#include <gui/guiconstants.h>
#include <gui/plugins/guiplugincontext.h>
#include <gui/trackselectioncontroller.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/fypaths.h>
#include <utils/mirage.h>
#include <utils/settings/settingsmanager.h>
#include <utils/utils.h>

#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDir>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QScopedValueRollback>

Q_LOGGING_CATEGORY(SUBSONIC_PLUGIN, "fy.subsonic.plugin")

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

constexpr auto PlaylistProperty = "SubsonicPlaylist";
//! Set while a mirrored playlist has edits the server has not saved, so they survive a restart.
constexpr auto UnsavedProperty = "SubsonicUnsaved";
//! Set on mirrored playlists the server does not let this user change, such as another user's.
constexpr auto ReadOnlyProperty = "SubsonicReadOnly";
constexpr int NotAuthorizedCode = 50;
constexpr int NotFoundCode      = 70;

namespace {
bool hasLatinTag(const Fooyin::Track& track)
{
    using namespace Fooyin::Subsonic;
    return std::ranges::any_of(std::array{TitleLatinTag, AlbumLatinTag, ArtistLatinTag, TrackArtistLatinTag,
                                          AlbumArtistLatinTag},
                               [&track](const char* tag) { return track.hasExtraTag(QString::fromLatin1(tag)); });
}

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
    m_audioLoader     = context.audioLoader;
    m_network         = context.networkAccess;

    // Rips left by a crash; uploads in progress live only as long as their dialog
    QDir{Utils::cachePath(u"uploads"_s)}.removeRecursively();

    m_settings->subscribe(Scripting::NameDisplaySetting, this, [this]() {
        // Views show script results of these tracks, so they are told that their names changed
        TrackList tracks;
        std::ranges::copy_if(m_library->libraryTracks(), std::back_inserter(tracks), hasLatinTag);
        if(!tracks.empty()) {
            m_library->updateTrackMetadata(tracks);
        }
    });

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
    // Changes with indexes only refresh the metadata of entries, for example after a sync
    QObject::connect(m_playlistHandler, &PlaylistHandler::tracksChanged, this,
                     [this](Playlist* playlist, const std::vector<int>& indexes) {
                         if(indexes.empty()) {
                             playlistEdited(playlist);
                         }
                     });
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

void SubsonicPlugin::initialise(const GuiPluginContext& context)
{
    new SubsonicPage(m_provider.get(), m_libraryManager, m_library, m_settings, this);
    m_nameDisplayPage = new NameDisplayPage(m_settings, this);

    // View > Names and Lyrics applies at once
    auto* viewMenu    = context.actionManager->actionContainer(Constants::Menus::View);
    m_nameDisplayMenu = new QMenu(tr("&Names and Lyrics"), viewMenu->menu());
    m_nameDisplayMenu->setStatusTip(tr("Show names and lyrics as the original, romanized, or both"));
    auto* nameDisplayGroup = new QActionGroup(m_nameDisplayMenu);
    for(const auto& [display, title] :
        {std::pair{Scripting::NameDisplay::Original, tr("&Original")},
         std::pair{Scripting::NameDisplay::Romanized, tr("&Romanized")},
         std::pair{Scripting::NameDisplay::Both, tr("Original &and romanized")}}) {
        auto* action = m_nameDisplayMenu->addAction(title);
        action->setCheckable(true);
        action->setData(static_cast<int>(display));
        nameDisplayGroup->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, display]() {
            m_settings->set(Scripting::NameDisplaySetting, static_cast<int>(display));
        });
    }
    const auto checkNameDisplay = [nameDisplayGroup](const QVariant& value) {
        for(QAction* action : nameDisplayGroup->actions()) {
            action->setChecked(action->data() == value.toInt());
        }
    };
    checkNameDisplay(m_settings->value(Scripting::NameDisplaySetting));
    m_settings->subscribe(Scripting::NameDisplaySetting, m_nameDisplayMenu, checkNameDisplay);
    viewMenu->addAction(m_nameDisplayMenu->menuAction(), Actions::Groups::Three);

    auto* fileMenu = context.actionManager->actionContainer(Constants::Menus::File);

    m_uploadAction = new QAction(tr("&Upload to Server…"), this);
    m_uploadAction->setStatusTip(tr("Upload local files to a server that accepts uploads"));
    auto* uploadCommand = context.actionManager->registerAction(m_uploadAction, "Subsonic.Upload");
    uploadCommand->setCategories({tr("File")});
    fileMenu->addAction(uploadCommand, Actions::Groups::One);
    QObject::connect(m_uploadAction, &QAction::triggered, this, [this]() { showUpload(); });

    m_ripAction = new QAction(tr("&Rip CD to Server…"), this);
    m_ripAction->setStatusTip(tr("Rip an audio CD to FLAC and upload it to a server that accepts uploads"));
    auto* ripCommand = context.actionManager->registerAction(m_ripAction, "Subsonic.RipCd");
    ripCommand->setCategories({tr("File")});
    fileMenu->addAction(ripCommand, Actions::Groups::One);
    QObject::connect(m_ripAction, &QAction::triggered, this, [this]() { ripCd(); });

    // The library's track menu offers both for the server of the selected tracks
    const auto addServerAction = [this, &context](QAction* action, const char* id, const QString& title) {
        context.trackSelection->registerTrackContextAction(
            this, TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection, id, title,
            [this, action, title](QMenu* menu, const TrackSelection& selection) {
                const QString server = selectionServer(selection.tracks);
                if(action->isVisible() && !server.isEmpty() && m_provider->canUpload(server)) {
                    auto* entry = new QAction(title, menu);
                    QObject::connect(entry, &QAction::triggered, this, [this, action, server]() {
                        action == m_uploadAction ? showUpload(server) : ripCd(server);
                    });
                    menu->addAction(entry);
                }
            },
            Constants::Menus::Context::Utilities);
    };
    addServerAction(m_uploadAction, "Subsonic.UploadToServer", tr("Upload to This Server…"));
    addServerAction(m_ripAction, "Subsonic.RipCdToServer", tr("Rip CD to This Server…"));

    // Uploads and romanized names exist only for servers that offer them, which is known once they have answered
    updateServerActions();
    QObject::connect(m_provider.get(), &SubsonicProvider::serverInfoChanged, this,
                     &SubsonicPlugin::updateServerActions);
    QObject::connect(m_libraryManager, &LibraryManager::libraryRemoved, this, &SubsonicPlugin::updateServerActions);
}

QString SubsonicPlugin::selectionServer(const TrackList& tracks) const
{
    QString server;
    for(const Track& track : tracks) {
        const QString trackServer = m_provider->libraryPathFor(track.filepath());
        if(trackServer.isEmpty() || (!server.isEmpty() && trackServer != server)) {
            return {};
        }
        server = trackServer;
    }
    return server;
}

std::vector<UploadDialog::Server> SubsonicPlugin::uploadServers() const
{
    std::vector<UploadDialog::Server> servers;
    for(const auto& [id, library] : m_libraryManager->allLibraries()) {
        if(m_provider->isProviderPath(library.path) && m_provider->canUpload(library.path)) {
            servers.push_back({.libraryPath = library.path, .name = library.name});
        }
    }
    return servers;
}

void SubsonicPlugin::updateServerActions()
{
    // Only servers with the romanization extension send romanized names, so without one there is nothing to choose
    const auto libraries    = m_libraryManager->allLibraries();
    const bool romanization = std::ranges::any_of(
        libraries, [this](const auto& library) { return m_provider->hasRomanization(library.second.path); });
    m_nameDisplayPage->setHidden(!romanization);
    m_nameDisplayMenu->menuAction()->setVisible(romanization);

    const bool canUpload = !uploadServers().empty();
    m_uploadAction->setVisible(canUpload);
    // The audio CD plugin, where it is built and enabled, rips the disc
    m_ripAction->setVisible(canUpload && qobject_cast<Cdda::DiscRipper*>(Mirage::discRipper()));
}

UploadDialog* SubsonicPlugin::createUploadDialog(const QString& libraryPath)
{
    const auto servers = uploadServers();
    if(servers.empty()) {
        return nullptr;
    }

    auto* dialog = new UploadDialog(m_provider.get(), m_network, servers, libraryPath, Utils::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &UploadDialog::uploaded, this, [this](const QString& server) {
        for(const auto& [id, library] : m_libraryManager->allLibraries()) {
            if(library.path == server) {
                m_library->refresh(library);
            }
        }
    });
    return dialog;
}

void SubsonicPlugin::showUpload(const QString& libraryPath)
{
    if(auto* dialog = createUploadDialog(libraryPath)) {
        dialog->setAudioLoader(m_audioLoader);
        dialog->show();
    }
}

void SubsonicPlugin::ripCd(const QString& libraryPath)
{
    auto* ripper = qobject_cast<Cdda::DiscRipper*>(Mirage::discRipper());
    if(!ripper || uploadServers().empty()) {
        return;
    }

    ripper->chooseDisc(Utils::getMainWindow(), [this, ripper, libraryPath](const TrackList& tracks, const QString& error) {
        if(tracks.empty()) {
            if(!error.isEmpty()) {
                QMessageBox::information(Utils::getMainWindow(), tr("Rip CD to Server"), error);
            }
            return;
        }
        if(auto* dialog = createUploadDialog(libraryPath)) {
            dialog->setDisc(ripper, tracks);
            dialog->show();
        }
    });
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

void SubsonicPlugin::playlistEdited(Playlist* playlist)
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

void SubsonicPlugin::playlistRemoved(const Playlist* playlist)
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

void SubsonicPlugin::deletePlaylist(const QString& key)
{
    QUrlQuery params;
    params.addQueryItem(u"id"_s, key.sliced(key.lastIndexOf('/'_L1) + 1));
    m_provider->call(key, u"deletePlaylist"_s, params,
                     [this, key](std::optional<QJsonObject> response, ApiError error) {
                         // Failed deletes are retried on the next sync
                         if(response || isRefused(error)) {
                             m_pendingDeletes.erase(key);
                         }
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
    // fooyin creates an empty "Default" playlist whenever there is none; it is uploaded once it gets tracks
    if(playlist->name() == "Default"_L1 && playlist->trackCount() == 0) {
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
    ++m_dirtyPlaylists[key];
    playlist->setExtraProperty(QString::fromLatin1(UnsavedProperty), u"1"_s);
    m_pushTimer.start();
}

void SubsonicPlugin::pushPlaylists()
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

        const QString serverId    = key.sliced(key.lastIndexOf('/'_L1) + 1);
        const QString songsPrefix = serverOf(key) + u"/song/"_s;

        // createPlaylist with a playlist id replaces its entries; tracks from elsewhere cannot be added
        QUrlQuery entries;
        entries.addQueryItem(u"playlistId"_s, serverId);
        for(const Track& track : (*it)->tracks()) {
            if(track.filepath().startsWith(songsPrefix)) {
                entries.addQueryItem(u"songId"_s, songId(track.filepath()));
            }
        }
        QUrlQuery rename;
        rename.addQueryItem(u"playlistId"_s, serverId);
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
        m_provider->call(key, u"createPlaylist"_s, entries,
                         [this, key, rename, finish](std::optional<QJsonObject> response, ApiError error) {
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
                                     // OpenSubsonic marks read-only playlists; Subsonic only lets owners edit
                                     const auto account  = m_provider->accountForPath(libraryPath);
                                     const QString owner = playlist.value("owner"_L1).toString();
                                     const bool readOnly = playlist.value("readonly"_L1).toBool()
                                                        || (account && !owner.isEmpty() && owner != account->username);

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

                                     const auto setReadOnly = [this, readOnly](Playlist* target) {
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
                if(key.startsWith(keyPrefix) && !serverKeys.contains(key)) {
                    m_playlistHandler->removePlaylist(playlist->id());
                }
            }
        });
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicplugin.cpp"

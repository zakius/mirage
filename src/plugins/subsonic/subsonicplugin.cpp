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
#include "subsonicplaylists.h"
#include "subsonicprovider.h"
#include "subsonicradio.h"
#include "subsonicuploaddialog.h"

#include "../cdda/cddadiscripper.h"

#include <core/coresettings.h>
#include <core/library/librarymanager.h>
#include <core/library/libraryprovider.h>
#include <core/library/musiclibrary.h>
#include <core/network/networkaccessmanager.h>
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

#include <QAbstractNetworkCache>
#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>

Q_LOGGING_CATEGORY(SUBSONIC_PLUGIN, "fy.subsonic.plugin")

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {
bool hasLatinTag(const Fooyin::Track& track)
{
    using namespace Fooyin::Subsonic;
    return std::ranges::any_of(std::array{TitleLatinTag, AlbumLatinTag, ArtistLatinTag, TrackArtistLatinTag,
                                          AlbumArtistLatinTag},
                               [&track](const char* tag) { return track.hasExtraTag(QString::fromLatin1(tag)); });
}

//! Names a Last.fm period, as oasis's top tracks station takes them.
QString periodTitle(const QString& period)
{
    static const std::map<QString, const char*> titles{
        {u"7day"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "Last 7 days")},
        {u"1month"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "Last month")},
        {u"3month"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "Last 3 months")},
        {u"6month"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "Last 6 months")},
        {u"12month"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "Last year")},
        {u"overall"_s, QT_TRANSLATE_NOOP("SubsonicPlugin", "All time")}};
    const auto it = titles.find(period);
    return it != titles.cend() ? QCoreApplication::translate("SubsonicPlugin", it->second) : period;
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
    m_playlistHandler  = context.playlistHandler;
    m_playerController = context.playerController;
    m_settings        = context.settingsManager;
    m_audioLoader     = context.audioLoader;
    m_network         = context.networkAccess;

    // Rips left by a crash; uploads in progress live only as long as their dialog
    QDir{Utils::cachePath(u"uploads"_s)}.removeRecursively();

    // Builds before 2026-11-02 let API replies, whose URLs carry the credentials, into the disk cache
    if(!m_settings->fileContains("Subsonic/NetworkCacheCleared"_L1)) {
        if(auto* cache = m_network->cache()) {
            cache->clear();
        }
        m_settings->fileSet("Subsonic/NetworkCacheCleared"_L1, true);
    }

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
    // The account goes before the library's tracks, so their removal from playlists reaches no server
    QObject::connect(m_libraryManager, &LibraryManager::libraryAboutToBeRemoved, this,
                     [this](const LibraryInfo& library) {
                         if(m_provider->isProviderPath(library.path)) {
                             m_provider->removeAccount(library.path);
                         }
                     });

    QObject::connect(context.playerController, &PlayerController::currentTrackChanged, this,
                     [this](const Track& track) { scrobble(track, false); });
    QObject::connect(context.playerController, &PlayerController::trackPlayed, this,
                     [this](const Track& track) { scrobble(track, true); });
    QObject::connect(m_library, &MusicLibrary::tracksStatsChanged, this, &SubsonicPlugin::writeStats);

    new PlaylistMirror(m_provider.get(), m_libraryManager, m_library, m_playlistHandler, this);
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

    // View > Show Lyrics Translations: translations that come with the lyrics, and on oasis one into the language
    // set on its dashboard, made only while this is on
    m_translationAction = new QAction(tr("Show Lyrics &Translations"), viewMenu->menu());
    m_translationAction->setCheckable(true);
    m_translationAction->setStatusTip(tr("Show a translation under each lyrics line, where the server has one"));
    m_translationAction->setChecked(m_settings->value(Scripting::LyricsTranslationSetting).toBool());
    QObject::connect(m_translationAction, &QAction::toggled, this,
                     [this](bool checked) { m_settings->set(Scripting::LyricsTranslationSetting, checked); });
    viewMenu->addAction(m_translationAction, Actions::Groups::Three);

    // Playback > Start Station, for oasis's Last.fm stations
    auto* playbackMenu = context.actionManager->actionContainer(Constants::Menus::Playback);
    m_stationMenu      = new QMenu(tr("Start &Station"), playbackMenu->menu());
    m_stationMenu->setStatusTip(tr("Play a radio the server builds from your listening"));
    playbackMenu->addAction(m_stationMenu->menuAction());

    auto* fileMenu = context.actionManager->actionContainer(Constants::Menus::File);

    m_uploadAction = new QAction(tr("&Upload to Server…"), this);
    m_uploadAction->setStatusTip(tr("Upload local files to a server that accepts uploads"));
    m_uploadAction->setProperty(Mirage::ShortcutFollowsVisibility, true);
    auto* uploadCommand = context.actionManager->registerAction(m_uploadAction, "Subsonic.Upload");
    uploadCommand->setCategories({tr("File")});
    fileMenu->addAction(uploadCommand, Actions::Groups::One);
    QObject::connect(m_uploadAction, &QAction::triggered, this, [this]() { showUpload(); });

    m_ripAction = new QAction(tr("&Rip CD to Server…"), this);
    m_ripAction->setStatusTip(tr("Rip an audio CD to FLAC and upload it to a server that accepts uploads"));
    m_ripAction->setProperty(Mirage::ShortcutFollowsVisibility, true);
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

    // Plays the server's songs like the selection, topped up as they play
    m_radio = new Radio(m_provider.get(), m_playlistHandler, m_playerController, this);
    QObject::connect(m_radio, &Radio::failed, this, [](const QString& message) {
        QMessageBox::information(Utils::getMainWindow(), tr("Start Radio"), message);
    });
    context.trackSelection->registerTrackContextAction(
        this, TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection, "Subsonic.StartRadio",
        tr("Start Radio"),
        [this](QMenu* menu, const TrackSelection& selection) {
            const QString server = selectionServer(selection.tracks);
            if(server.isEmpty()) {
                return;
            }
            // oasis offers a choice of where the songs come from
            const auto sources = m_provider->radioOptions(server).sources;
            if(sources.size() < 2) {
                auto* entry = new QAction(tr("Start Radio"), menu);
                QObject::connect(entry, &QAction::triggered, this,
                                 [this, tracks = selection.tracks]() { startRadio(tracks); });
                menu->addAction(entry);
                return;
            }
            auto* radioMenu = menu->addMenu(tr("Start Radio"));
            for(const auto& source : sources) {
                QObject::connect(radioMenu->addAction(source.name), &QAction::triggered, this,
                                 [this, tracks = selection.tracks, id = source.id]() { startRadio(tracks, id); });
            }
        },
        Constants::Menus::Context::Utilities);

    // Stars the albums or artists of the selected tracks, or unstars them where all of them are starred
    const auto addStarAction = [this, &context](const char* id, bool album) {
        context.trackSelection->registerTrackContextAction(
            this, TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection, id,
            album ? tr("Star Album") : tr("Star Artist"),
            [this, album](QMenu* menu, const TrackSelection& selection) {
                const QString server = selectionServer(selection.tracks);
                // Playlists keep the copies they loaded, which may predate the sync that stored the ids
                TrackList tracks;
                std::ranges::transform(selection.tracks, std::back_inserter(tracks), [this](const Track& track) {
                    const Track libraryTrack = m_library->trackForId(track.id());
                    return libraryTrack.isValid() ? libraryTrack : track;
                });
                QStringList ids;
                for(const Track& track : tracks) {
                    ids.append(album ? track.extraTag(QString::fromLatin1(AlbumIdTag)) : artistIds(track));
                }
                ids.removeDuplicates();
                if(server.isEmpty() || ids.isEmpty()) {
                    return;
                }

                const char* tag     = album ? AlbumStarredTag : ArtistStarredTag;
                const bool starred  = std::ranges::all_of(
                    tracks, [tag](const Track& track) { return track.hasExtraTag(QString::fromLatin1(tag)); });
                const bool several  = ids.size() > 1;
                const QString title = album ? (starred ? (several ? tr("Unstar Albums") : tr("Unstar Album"))
                                                       : (several ? tr("Star Albums") : tr("Star Album")))
                                            : (starred ? (several ? tr("Unstar Artists") : tr("Unstar Artist"))
                                                       : (several ? tr("Star Artists") : tr("Star Artist")));
                auto* entry         = new QAction(title, menu);
                QObject::connect(entry, &QAction::triggered, this,
                                 [this, server, ids, album, starred]() { star(server, ids, album, !starred); });
                menu->addAction(entry);
            },
            Constants::Menus::Context::Utilities);
    };
    addStarAction("Subsonic.StarAlbum", true);
    addStarAction("Subsonic.StarArtist", false);

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
    m_translationAction->setVisible(std::ranges::any_of(
        libraries, [this](const auto& library) { return m_provider->hasLyricsKinds(library.second.path); }));
    updateStations();

    const bool canUpload = !uploadServers().empty();
    m_uploadAction->setVisible(canUpload);
    // The audio CD plugin, where it is built and enabled, rips the disc
    m_ripAction->setVisible(canUpload && qobject_cast<Cdda::DiscRipper*>(Mirage::discRipper()));
}

void SubsonicPlugin::updateStations()
{
    m_stationMenu->clear();
    for(const auto& library : m_libraryManager->allLibraries() | std::views::values) {
        const auto stations = m_provider->radioOptions(library.path).stations;
        if(stations.empty()) {
            continue;
        }
        for(const auto& station : stations) {
            const auto start = [this, server = library.path, station](const QString& period, const QString& name) {
                m_radio->startStation(server, station.id, period, name);
            };
            if(station.periods.isEmpty()) {
                QObject::connect(m_stationMenu->addAction(station.name), &QAction::triggered, this,
                                 [start, name = station.name]() { start({}, name); });
                continue;
            }
            auto* periodMenu = m_stationMenu->addMenu(station.name);
            for(const QString& period : station.periods) {
                const QString title = periodTitle(period);
                auto* action        = periodMenu->addAction(title);
                QObject::connect(action, &QAction::triggered, this, [start, period, name = station.name, title]() {
                    start(period, u"%1 (%2)"_s.arg(name, title));
                });
            }
        }
        break;
    }
    m_stationMenu->menuAction()->setVisible(!m_stationMenu->isEmpty());
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

void SubsonicPlugin::startRadio(const TrackList& tracks, const QString& source)
{
    const QString server = selectionServer(tracks);
    if(server.isEmpty() || tracks.empty()) {
        return;
    }

    // Library copies carry the album and artist ids; a playlist may hold older ones
    TrackList current;
    std::ranges::transform(tracks, std::back_inserter(current), [this](const Track& track) {
        const Track libraryTrack = m_library->trackForId(track.id());
        return libraryTrack.isValid() ? libraryTrack : track;
    });
    const auto shared = [&current](const auto& ids) {
        const QStringList first = ids(current.front());
        return !first.isEmpty() && std::ranges::all_of(current, [&](const Track& track) { return ids(track) == first; })
                 ? first.front()
                 : QString{};
    };
    const Track& seed = current.front();

    if(current.size() > 1) {
        if(const QString album
           = shared([](const Track& track) { return track.extraTag(QString::fromLatin1(AlbumIdTag)); });
           !album.isEmpty()) {
            m_radio->start(server, album, seed.album(), {}, source);
            return;
        }
        if(const QString artist = shared([](const Track& track) { return artistIds(track); }); !artist.isEmpty()) {
            m_radio->start(server, artist, seed.effectiveAlbumArtist(), {}, source);
            return;
        }
    }
    m_radio->start(server, songId(seed.filepath()), seed.title(), seed, source);
}

void SubsonicPlugin::star(const QString& server, const QStringList& ids, bool album, bool starred)
{
    QUrlQuery params;
    for(const QString& id : ids) {
        params.addQueryItem(album ? u"albumId"_s : u"artistId"_s, id);
    }

    const QString method = starred ? u"star"_s : u"unstar"_s;
    m_provider->callForm(
        server, method, params,
        [this, server, ids, album, starred, method](std::optional<QJsonObject> response, ApiError error) {
            if(!response) {
                logError(method, response, error);
                return;
            }

            // The library shows the change at once; the next sync reads it back from the server
            const char* tag = album ? AlbumStarredTag : ArtistStarredTag;
            TrackList changed;
            for(Track track : m_library->libraryTracks()) {
                const QStringList trackIds = album ? track.extraTag(QString::fromLatin1(AlbumIdTag)) : artistIds(track);
                if(track.hasExtraTag(QString::fromLatin1(tag)) != starred
                   && m_provider->libraryPathFor(track.filepath()) == server
                   && std::ranges::any_of(trackIds, [&ids](const QString& id) { return ids.contains(id); })) {
                    setStarred(track, tag, starred);
                    changed.push_back(track);
                }
            }
            if(!changed.empty()) {
                m_library->updateTrackMetadata(changed);
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

} // namespace Fooyin::Subsonic

#include "moc_subsonicplugin.cpp"

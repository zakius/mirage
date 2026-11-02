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

#include "subsonicradio.h"

#include "subsonicapi.h"
#include "subsonicprovider.h"

#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(SUBSONIC_RADIO, "fy.subsonic.radio")

using namespace Qt::StringLiterals;

namespace {
//! Songs asked for at a time.
constexpr auto RadioCount = 50;
//! Songs left after the playing one when the radio asks for more.
constexpr auto TopUpAhead = 5;
} // namespace

namespace Fooyin::Subsonic {
Radio::Radio(SubsonicProvider* provider, PlaylistHandler* playlistHandler, PlayerController* playerController,
             QObject* parent)
    : QObject{parent}
    , m_provider{provider}
    , m_playlistHandler{playlistHandler}
    , m_playerController{playerController}
{
    QObject::connect(m_playerController, &PlayerController::playlistTrackChanged, this, &Radio::topUp);
}

void Radio::start(const QString& server, const QString& seedId, const QString& name, const Track& first)
{
    QUrlQuery params;
    params.addQueryItem(u"id"_s, seedId);
    params.addQueryItem(u"count"_s, QString::number(RadioCount));
    m_provider->call(server, u"getSimilarSongs"_s, params,
                     [this, server, name, first](std::optional<QJsonObject> response, const ApiError& error) {
                         if(!response) {
                             Q_EMIT failed(tr("The server could not start a radio: %1").arg(error.message));
                             return;
                         }

                         m_station = Station{.server = server};
                         TrackList tracks;
                         if(first.isValid()) {
                             tracks = unseen({first});
                         }
                         const TrackList similar = unseen(similarSongs(*response, m_provider->libraryPathFor(server)));
                         if(similar.empty()) {
                             m_station.reset();
                             Q_EMIT failed(tr("The server found no songs like this."));
                             return;
                         }
                         tracks.insert(tracks.end(), similar.cbegin(), similar.cend());

                         Playlist* playlist = m_playlistHandler->createTempPlaylist(tr("Radio: %1").arg(name), tracks);
                         if(!playlist) {
                             m_station.reset();
                             return;
                         }
                         m_station->playlistId = playlist->id();
                         m_playerController->startPlayback(playlist);
                     });
}

void Radio::topUp(const PlaylistTrack& current)
{
    if(!m_station || m_station->fetching || current.playlistId != m_station->playlistId) {
        return;
    }
    const Playlist* playlist = m_playlistHandler->playlistById(m_station->playlistId);
    if(!playlist) {
        m_station.reset();
        return;
    }
    if(playlist->trackCount() - 1 - current.indexInPlaylist > TopUpAhead) {
        return;
    }

    // Each top-up is seeded from the last song, so the radio drifts on
    QUrlQuery params;
    params.addQueryItem(u"id"_s, songId(playlist->tracks().back().filepath()));
    params.addQueryItem(u"count"_s, QString::number(RadioCount));
    m_station->fetching = true;
    m_provider->call(m_station->server, u"getSimilarSongs"_s, params,
                     [this, playlistId = m_station->playlistId](std::optional<QJsonObject> response, ApiError error) {
                         if(!m_station || m_station->playlistId != playlistId) {
                             return;
                         }
                         m_station->fetching = false;
                         if(!response) {
                             qCWarning(SUBSONIC_RADIO) << "Failed to top up the radio:" << error.message;
                             return;
                         }
                         const TrackList tracks
                             = unseen(similarSongs(*response, m_provider->libraryPathFor(m_station->server)));
                         if(!tracks.empty()) {
                             m_playlistHandler->appendToPlaylist(playlistId, tracks);
                         }
                     });
}

TrackList Radio::unseen(const TrackList& tracks)
{
    TrackList result;
    for(const Track& track : tracks) {
        if(m_station->songIds.emplace(songId(track.filepath())).second) {
            result.push_back(track);
        }
    }
    return result;
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicradio.cpp"

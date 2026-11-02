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

void Radio::start(const QString& server, const QString& seedId, const QString& name, const Track& first,
                  const QString& source)
{
    QUrlQuery params;
    params.addQueryItem(u"id"_s, seedId);
    params.addQueryItem(u"count"_s, QString::number(RadioCount));
    if(!source.isEmpty()) {
        params.addQueryItem(u"oasis"_s, u"1"_s);
        params.addQueryItem(u"oasisSource"_s, source);
    }
    m_provider->call(server, u"getSimilarSongs"_s, params,
                     [this, server, name, first, source](std::optional<QJsonObject> response, const ApiError& error) {
                         if(!response) {
                             Q_EMIT failed(tr("The server could not start a radio: %1").arg(error.message));
                             return;
                         }

                         stop();
                         m_station = Station{.server = server, .source = source};
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
                         play(name, tracks);
                     });
}

void Radio::startStation(const QString& server, const QString& station, const QString& period, const QString& name)
{
    QUrlQuery params;
    params.addQueryItem(u"oasis"_s, u"1"_s);
    params.addQueryItem(u"oasisStation"_s, station);
    if(!period.isEmpty()) {
        params.addQueryItem(u"oasisPeriod"_s, period);
    }
    m_provider->call(server, u"createPlaylist"_s, params,
                     [this, server, name](std::optional<QJsonObject> response, const ApiError& error) {
                         if(!response) {
                             Q_EMIT failed(tr("The server could not start the station: %1").arg(error.message));
                             return;
                         }

                         stop();
                         const QJsonObject playlist = response->value("playlist"_L1).toObject();
                         m_station = Station{.server = server, .stationId = playlist.value("id"_L1).toString()};
                         const TrackList tracks = unseen(similarSongs(
                             QJsonObject{{u"similarSongs"_s, QJsonObject{{u"song"_s, playlist.value("entry"_L1)}}}},
                             m_provider->libraryPathFor(server)));
                         if(tracks.empty()) {
                             stop();
                             Q_EMIT failed(tr("The station has no songs yet."));
                             return;
                         }
                         play(name, tracks);
                     });
}

void Radio::play(const QString& name, const TrackList& tracks)
{
    Playlist* playlist = m_playlistHandler->createTempPlaylist(tr("Radio: %1").arg(name), tracks);
    if(!playlist) {
        stop();
        return;
    }
    m_station->playlistId = playlist->id();
    m_playerController->startPlayback(playlist);
}

void Radio::stop()
{
    if(m_station && !m_station->stationId.isEmpty()) {
        QUrlQuery params;
        params.addQueryItem(u"id"_s, m_station->stationId);
        m_provider->call(m_station->server, u"deletePlaylist"_s, params, [](std::optional<QJsonObject>, ApiError) { });
    }
    m_station.reset();
}

void Radio::topUp(const PlaylistTrack& current)
{
    if(!m_station || m_station->fetching || current.playlistId != m_station->playlistId) {
        return;
    }
    const Playlist* playlist = m_playlistHandler->playlistById(m_station->playlistId);
    if(!playlist) {
        stop();
        return;
    }
    if(playlist->trackCount() - 1 - current.indexInPlaylist > TopUpAhead) {
        return;
    }

    // A station's server playlist refills as its songs play; a song radio asks for songs like its last one, so it
    // drifts on
    QUrlQuery params;
    QString method;
    if(!m_station->stationId.isEmpty()) {
        method = u"getPlaylist"_s;
        params.addQueryItem(u"id"_s, m_station->stationId);
    }
    else {
        method = u"getSimilarSongs"_s;
        params.addQueryItem(u"id"_s, songId(playlist->tracks().back().filepath()));
        params.addQueryItem(u"count"_s, QString::number(RadioCount));
        if(!m_station->source.isEmpty()) {
            params.addQueryItem(u"oasis"_s, u"1"_s);
            params.addQueryItem(u"oasisSource"_s, m_station->source);
        }
    }
    m_station->fetching = true;
    m_provider->call(m_station->server, method, params,
                     [this, playlistId = m_station->playlistId](std::optional<QJsonObject> response, ApiError error) {
                         if(!m_station || m_station->playlistId != playlistId) {
                             return;
                         }
                         m_station->fetching = false;
                         if(!response) {
                             qCWarning(SUBSONIC_RADIO) << "Failed to top up the radio:" << error.message;
                             return;
                         }
                         const QJsonValue entries = response->value("playlist"_L1).toObject().value("entry"_L1);
                         const QJsonObject songs
                             = response->contains("playlist"_L1)
                                 ? QJsonObject{{u"similarSongs"_s, QJsonObject{{u"song"_s, entries}}}}
                                 : *response;
                         const TrackList tracks
                             = unseen(similarSongs(songs, m_provider->libraryPathFor(m_station->server)));
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

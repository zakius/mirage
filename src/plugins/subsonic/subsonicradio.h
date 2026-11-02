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

#include <core/track.h>
#include <utils/id.h>

#include <QObject>

#include <optional>
#include <set>

namespace Fooyin {
class PlayerController;
class PlaylistHandler;
struct PlaylistTrack;

namespace Subsonic {
class SubsonicProvider;

/*!
 * Plays a server's similar songs (getSimilarSongs) as a temporary playlist, topped up from its last song while it
 * plays, or an oasis station, a radio playlist the server refills. One radio runs at a time; starting another
 * replaces it.
 */
class Radio : public QObject
{
    Q_OBJECT

public:
    Radio(SubsonicProvider* provider, PlaylistHandler* playlistHandler, PlayerController* playerController,
          QObject* parent = nullptr);

    /*!
     * Starts a radio on the server of @p server from the song, album or artist with the server id @p seedId, named
     * after @p name. @p first, if valid, plays first. @p source picks an oasis radio source (extension `oasis` v2).
     */
    void start(const QString& server, const QString& seedId, const QString& name, const Track& first = {},
               const QString& source = {});
    //! Starts the oasis station @p station, over @p period for one that takes a period, named @p name.
    void startStation(const QString& server, const QString& station, const QString& period, const QString& name);

Q_SIGNALS:
    //! Emitted when a radio could not start, with the reason to show.
    void failed(const QString& message);

private:
    void topUp(const PlaylistTrack& current);
    //! Replaces the running radio with one playing @p tracks as @p name.
    void play(const QString& name, const TrackList& tracks);
    //! Ends the running radio, removing its station playlist from the server.
    void stop();
    //! Keeps the songs of @p tracks the radio hasn't played or queued yet.
    TrackList unseen(const TrackList& tracks);

    SubsonicProvider* m_provider;
    PlaylistHandler* m_playlistHandler;
    PlayerController* m_playerController;

    struct Station
    {
        QString server;
        //! The oasis radio source, if one was picked.
        QString source;
        //! The server's playlist of a station, which it refills as songs play.
        QString stationId;
        UId playlistId;
        std::set<QString> songIds;
        bool fetching{false};
    };
    std::optional<Station> m_station;
};
} // namespace Subsonic
} // namespace Fooyin

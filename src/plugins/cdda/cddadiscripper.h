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

#include <QStringList>
#include <QtPlugin>

#include <functional>

class QWidget;

namespace Fooyin::Cdda {
//! Extra properties of the tracks chooseDisc() returns: the MusicBrainz disc id and TOC string.
constexpr auto DiscIdProperty  = "_CDDA_DISC_ID";
constexpr auto DiscTocProperty = "_CDDA_DISC_TOC";

//! Returns the extra property @p name of a track from DiscRipper::chooseDisc(), or an empty string.
inline QString discProperty(const Track& track, const char* name)
{
    const auto properties = track.extraProperties();
    const QString* value  = properties.find(QString::fromLatin1(name));
    return value ? *value : QString{};
}

/*!
 * Rips audio CDs for other plugins. The audio CD plugin registers itself with `Mirage::setDiscRipper()`; users find
 * it with `qobject_cast<Cdda::DiscRipper*>(Mirage::discRipper())`. Everything here is inline, so users need not link
 * the plugin.
 */
class DiscRipper
{
public:
    struct Rip
    {
        //! The FLAC file of each track, in the order of the tracks.
        QStringList files;
        //! Why the rip did not complete; empty on success.
        QString error;
        bool cancelled{false};
        //! True if AccurateRip knows the disc and a track did not match.
        bool accurateRipMismatch{false};
    };

    virtual ~DiscRipper() = default;

    /*!
     * Finds an inserted audio CD, asking the user if there are several, and calls @p done with its tracks (with
     * CD-Text where the disc has it), or with no tracks and a message.
     */
    virtual void chooseDisc(QWidget* parent, std::function<void(TrackList tracks, QString error)> done) = 0;
    /*!
     * Rips @p tracks, from chooseDisc(), to FLAC files in @p folder, with the converter's progress dialog. Verifies
     * them with AccurateRip where the disc is known there, and shows that result.
     */
    virtual void ripToFolder(const TrackList& tracks, const QString& folder, std::function<void(Rip)> done) = 0;
};
} // namespace Fooyin::Cdda

Q_DECLARE_INTERFACE(Fooyin::Cdda::DiscRipper, "org.fooyin.mirage.DiscRipper/1.0")

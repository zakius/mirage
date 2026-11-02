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

#include "fycore_export.h"

#include <QStringList>

#include <cstdint>

namespace Fooyin {
class Track;
} // namespace Fooyin

namespace Fooyin::Scripting {
//! Extra tags holding romanized names, set where they differ from the original (see the Subsonic plugin).
constexpr auto TitleLatinTag  = "TITLELATIN";
constexpr auto AlbumLatinTag  = "ALBUMLATIN";
//! One name per artist, in the order of Track::artists(); an artist without a romanized form keeps its name.
constexpr auto ArtistLatinTag = "ARTISTLATIN";
//! The romanized names of the artists that are not album artists, as %trackartist% lists them.
constexpr auto TrackArtistLatinTag = "TRACKARTISTLATIN";
//! One name per album artist, in the order of Track::albumArtists().
constexpr auto AlbumArtistLatinTag = "ALBUMARTISTLATIN";

/*!
 * String setting key holding the NameDisplay of $display_name(). The core applies it with setNameDisplay(); places
 * that show names outside scripts subscribe to it. (The key dates from when the Subsonic page owned it.)
 */
constexpr auto NameDisplaySetting = "Subsonic/NameDisplay";

//! Bool setting key: whether server lyrics show translations under their lines, off unless asked for.
constexpr auto LyricsTranslationSetting = "Subsonic/LyricsTranslation";

//! How $display_name() shows names with a romanized form.
enum class NameDisplay : uint8_t
{
    Original = 0,
    Romanized,
    Both,
};

/*!
 * Returns @p original as @p display asks; a missing romanized form shows the original. Multiple values (joined by
 * Constants::UnitSeparator, as from %<artist>%) are paired one by one if both sides have as many.
 */
FYCORE_EXPORT QString displayName(const QString& original, const QString& romanized, NameDisplay display);
//! Returns each of @p originals with the romanized form at the same index, as displayName() does.
FYCORE_EXPORT QStringList displayNames(const QStringList& originals, const QStringList& romanized,
                                       NameDisplay display);

//! A track's names as nameDisplay() asks, for places that show them without a script.
struct TrackDisplayNames
{
    QString title;
    QString album;
    QStringList artists;
    QStringList albumArtists;
};
FYCORE_EXPORT TrackDisplayNames trackDisplayNames(const Track& track);

//! Sets how $display_name() shows names. It shows originals until this is called; safe from any thread.
FYCORE_EXPORT void setNameDisplay(NameDisplay display);
//! Returns the display set with setNameDisplay().
FYCORE_EXPORT NameDisplay nameDisplay();
} // namespace Fooyin::Scripting

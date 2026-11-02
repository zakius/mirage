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

#include "fyutils_export.h"

#include <utils/id.h>

class QObject;

namespace Fooyin::Mirage {
/*!
 * The local media gate: returns @c true for the ids of actions, track menu entries, settings pages and widgets that
 * read, write or open local files, or local media such as CDs. mirage plays Subsonic servers only, so these are
 * hidden (and their actions disabled) instead of removed. Removing an id from the list brings its feature back.
 */
FYUTILS_EXPORT bool isLocalMediaId(const Id& id);

/*!
 * Set to @c true on an action whose command is listed in Settings > Shortcuts only while the action is visible, such
 * as an action that exists only for servers offering a feature.
 */
constexpr auto ShortcutFollowsVisibility = "fyShortcutFollowsVisibility";

/*!
 * The audio CD plugin registers its ripper here (a `Cdda::DiscRipper`, see `plugins/cdda/cddadiscripper.h`), so the
 * Subsonic plugin can rip a CD to a server without either plugin linking the other. @c nullptr where there is none.
 */
FYUTILS_EXPORT void setDiscRipper(QObject* ripper);
[[nodiscard]] FYUTILS_EXPORT QObject* discRipper();

//! Extra tags of Subsonic songs: the server ids of their album, artist and album artists (OpenSubsonic `albumArtists`).
constexpr auto AlbumIdTag       = "SUBSONIC_ALBUMID";
constexpr auto ArtistIdTag      = "SUBSONIC_ARTISTID";
constexpr auto AlbumArtistIdTag = "SUBSONIC_ALBUMARTISTID";
//! Extra tags set to "1" on songs whose album, or album artist, the user starred.
constexpr auto AlbumStarredTag  = "ALBUMSTARRED";
constexpr auto ArtistStarredTag = "ARTISTSTARRED";
} // namespace Fooyin::Mirage

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

#include <utils/mirage.h>

#include <QStringList>

using namespace Qt::StringLiterals;

namespace Fooyin::Mirage {
bool isLocalMediaId(const Id& id)
{
    static const QStringList ids{
        // File menu
        u"File.AddFiles"_s, u"File.AddFolders"_s, u"File.LoadPlaylist"_s, u"File.SavePlaylist"_s,
        u"File.SaveAllPlaylists"_s, u"File.OpenAudioCd"_s,
        // Track menus
        u"Tracks.OpenFolder"_s, u"Tracks.CopyLocation"_s, u"Tracks.CopyDirectoryPath"_s, u"Tracks.SearchArtwork"_s,
        u"Tracks.ExportArtwork"_s, u"Tracks.RemoveArtwork"_s, u"Fooyin.Menu.Artwork.Attach"_s, u"Fooyin.Menu.Tagging"_s,
        u"Tracks.ReloadTags"_s, u"Tracks.ReloadModifiedTags"_s, u"Tracks.LookupMetadata"_s,
        u"Tracks.LookupMetadataByDiscToc"_s, u"Tracks.LookupMetadataById"_s, u"Fooyin.Menu.Convert"_s,
        u"Tracks.VerifyIntegrity"_s, u"Tracks.VerifyAccurateRipAlbum"_s, u"Fooyin.Menu.Lyrics"_s, u"Lyrics.Edit"_s,
        u"Lyrics.SearchQuick"_s, u"Lyrics.Search"_s,
        // Settings pages
        u"Fooyin.Page.Playlist.Saving"_s, u"Fooyin.Page.Lyrics.Saving"_s, u"Fooyin.Page.Interface.Artwork.Download"_s,
        u"Fooyin.Page.Integrations.System"_s, u"Fooyin.Page.Playback.Decoding"_s, u"Fooyin.Page.Tagging.QuickTagger"_s,
        // Widgets
        u"DirectoryBrowser"_s};

    const QString name = id.name();
    return ids.contains(name) || name.startsWith("Tracks.Convert"_L1) || name.startsWith("Tagging.QuickTagger"_L1);
}
} // namespace Fooyin::Mirage

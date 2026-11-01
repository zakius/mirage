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

#include <gtest/gtest.h>

namespace Fooyin::Testing {
TEST(MirageTest, GatesActionsMenusAndPagesForLocalMedia)
{
    for(const char* id : {"File.AddFiles", "File.AddFolders", "File.LoadPlaylist", "File.SavePlaylist",
                          "File.SaveAllPlaylists", "File.OpenAudioCd", "Tracks.OpenFolder", "Tracks.ExportArtwork",
                          "Fooyin.Menu.Tagging", "Tracks.ReloadTags", "Tracks.Convert.Preset.abc",
                          "Tagging.QuickTagger.Mood.Calm", "Lyrics.Edit", "Fooyin.Page.Playlist.Saving",
                          "Fooyin.Page.Playback.Decoding", "DirectoryBrowser"}) {
        EXPECT_TRUE(Mirage::isLocalMediaId(Id{id})) << id;
    }
}

TEST(MirageTest, KeepsServerTrackActions)
{
    for(const char* id : {"File.AddStreamUrl", "File.NewPlaylist", "Tracks.OpenProperties", "Tracks.ViewArtwork",
                          "Tracks.AddToCurrent", "Tracks.ToggleLove", "Fooyin.Page.Library.Subsonic",
                          "Fooyin.Page.Lyrics.Sources", "TagEditor", "Lyrics"}) {
        EXPECT_FALSE(Mirage::isLocalMediaId(Id{id})) << id;
    }
}
} // namespace Fooyin::Testing

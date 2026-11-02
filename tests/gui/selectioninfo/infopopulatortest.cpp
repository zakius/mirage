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

#include "gui/selectioninfo/infopopulator.h"

#include <utils/mirage.h>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
QString label(const InfoData& data, const QString& key)
{
    const auto it = data.nodes.find(key);
    return it != data.nodes.cend() ? it->second.name() : QString{};
}

InfoDataPtr populate(const TrackList& tracks)
{
    const std::vector<SelectionInfoField> fields{{.id = 0, .name = u"Artist"_s, .scriptField = u"artist"_s},
                                                 {.id = 1, .name = u"Title"_s, .scriptField = u"title"_s},
                                                 {.id = 2, .name = u"Album"_s, .scriptField = u"album"_s}};
    InfoPopulator populator;
    InfoDataPtr result;
    QObject::connect(&populator, &InfoPopulator::populated, [&result](const InfoDataPtr& data) { result = data; });
    populator.run(InfoItem::Metadata, tracks, fields, {}, 1, populator.prepareWork());
    return result;
}

Track serverTrack(bool starred)
{
    Track track{u"subsonic://u@host/song/s1"_s};
    track.setTitle(u"Song"_s);
    track.setArtists({u"Artist"_s});
    track.setAlbum(u"Album"_s);
    track.replaceExtraTag(QString::fromLatin1(Mirage::AlbumIdTag), u"al1"_s);
    track.replaceExtraTag(QString::fromLatin1(Mirage::ArtistIdTag), u"ar1"_s);
    track.setLoved(starred);
    if(starred) {
        track.replaceExtraTag(QString::fromLatin1(Mirage::AlbumStarredTag), u"1"_s);
        track.replaceExtraTag(QString::fromLatin1(Mirage::ArtistStarredTag), u"1"_s);
    }
    return track;
}
} // namespace

TEST(InfoPopulatorTest, StarsTheLabelsOfServerSongs)
{
    const auto unstarred = populate({serverTrack(false)});
    ASSERT_TRUE(unstarred);
    EXPECT_EQ(label(*unstarred, u"Metadata/ARTIST"_s), u"Artist ☆"_s);
    EXPECT_EQ(label(*unstarred, u"Metadata/TITLE"_s), u"Title ☆"_s);
    EXPECT_EQ(label(*unstarred, u"Metadata/ALBUM"_s), u"Album ☆"_s);

    const auto starred = populate({serverTrack(true)});
    ASSERT_TRUE(starred);
    EXPECT_EQ(label(*starred, u"Metadata/ARTIST"_s), u"Artist ★"_s);
    EXPECT_EQ(label(*starred, u"Metadata/TITLE"_s), u"Title ★"_s);
    EXPECT_EQ(label(*starred, u"Metadata/ALBUM"_s), u"Album ★"_s);
}

TEST(InfoPopulatorTest, LeavesOtherSongsUnstarred)
{
    Track track{u"/music/song.flac"_s};
    track.setTitle(u"Song"_s);
    const auto data = populate({track});
    ASSERT_TRUE(data);
    EXPECT_EQ(label(*data, u"Metadata/TITLE"_s), u"Title"_s);
}
} // namespace Fooyin::Testing

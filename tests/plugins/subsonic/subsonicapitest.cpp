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

#include "subsonicapi.h"

#include <core/scripting/scriptparser.h>

#include <QJsonDocument>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
QJsonObject response(const char* json)
{
    return QJsonDocument::fromJson(json).object();
}
} // namespace

TEST(SubsonicApiTest, ScrobblesLocallyUnlessTheServerScrobbles)
{
    EXPECT_TRUE(Subsonic::scrobblesLocally(std::nullopt, std::nullopt));
    EXPECT_TRUE(Subsonic::scrobblesLocally(std::nullopt, false));
    EXPECT_FALSE(Subsonic::scrobblesLocally(std::nullopt, true));
}

TEST(SubsonicApiTest, UserChoiceWinsOverTheServer)
{
    EXPECT_TRUE(Subsonic::scrobblesLocally(true, true));
    EXPECT_FALSE(Subsonic::scrobblesLocally(false, false));
    EXPECT_FALSE(Subsonic::scrobblesLocally(false, std::nullopt));
}

TEST(SubsonicApiTest, ReadsScrobblingEnabledFromGetUser)
{
    EXPECT_EQ(Subsonic::serverScrobbles(response(R"({"user": {"username": "a", "scrobblingEnabled": true}})")), true);
    EXPECT_EQ(Subsonic::serverScrobbles(response(R"({"user": {"username": "a", "scrobblingEnabled": false}})")),
              false);
    EXPECT_EQ(Subsonic::serverScrobbles(response(R"({"user": {"username": "a"}})")), std::nullopt);
    EXPECT_EQ(Subsonic::serverScrobbles({}), std::nullopt);
}

TEST(SubsonicApiTest, FindsExtensionsByNameAndVersion)
{
    const QJsonObject extensions = response(R"({"openSubsonicExtensions": [
        {"name": "songLyrics", "versions": [1, 2]},
        {"name": "romanization", "versions": [1]}
    ]})");

    EXPECT_TRUE(Subsonic::hasExtension(extensions, u"romanization"_s, 1));
    EXPECT_TRUE(Subsonic::hasExtension(extensions, u"songLyrics"_s, 2));
    EXPECT_FALSE(Subsonic::hasExtension(extensions, u"romanization"_s, 2));
    EXPECT_FALSE(Subsonic::hasExtension(response(R"({"openSubsonicExtensions": []})"), u"romanization"_s, 1));
    EXPECT_FALSE(Subsonic::hasExtension({}, u"romanization"_s, 1));
}
TEST(SubsonicApiTest, KeepsRomanizedNamesInExtraTags)
{
    const Track track = Subsonic::songToTrack(response(R"({
        "id": "s1", "title": "夜に駆ける", "titleLatin": "Yoru ni Kakeru",
        "album": "THE BOOK", "artist": "YOASOBI",
        "artists": [{"id": "a1", "name": "YOASOBI"}, {"id": "a2", "name": "幾田りら", "nameLatin": "Ikuta Lilas"}],
        "albumArtists": [{"id": "a1", "name": "YOASOBI"}]
    })"),
                                              u"subsonic://u@host"_s);

    EXPECT_EQ(track.title(), u"夜に駆ける"_s);
    EXPECT_EQ(track.extraTag(u"TITLELATIN"_s), QStringList{u"Yoru ni Kakeru"_s});
    EXPECT_FALSE(track.hasExtraTag(u"ALBUMLATIN"_s));
    EXPECT_EQ(track.artists(), (QStringList{u"YOASOBI"_s, u"幾田りら"_s}));
    EXPECT_EQ(track.extraTag(u"ARTISTLATIN"_s), (QStringList{u"YOASOBI"_s, u"Ikuta Lilas"_s}));
    EXPECT_EQ(track.extraTag(u"TRACKARTISTLATIN"_s), QStringList{u"Ikuta Lilas"_s});
    EXPECT_FALSE(track.hasExtraTag(u"ALBUMARTISTLATIN"_s));

    ScriptParser parser;
    EXPECT_EQ(parser.evaluate(u"%titlelatin%"_s, track), u"Yoru ni Kakeru"_s);
}

TEST(SubsonicApiTest, ReadsRomanizedNamesOfPlainArtistFields)
{
    const Track track = Subsonic::songToTrack(response(R"({
        "id": "s1", "title": "Song", "artist": "東京事変", "artistLatin": "Tokyo Jihen",
        "displayAlbumArtist": "東京事変", "displayAlbumArtistLatin": "Tokyo Jihen"
    })"),
                                              u"subsonic://u@host"_s);

    EXPECT_FALSE(track.hasExtraTag(u"TITLELATIN"_s));
    EXPECT_EQ(track.extraTag(u"ARTISTLATIN"_s), QStringList{u"Tokyo Jihen"_s});
    EXPECT_EQ(track.extraTag(u"ALBUMARTISTLATIN"_s), QStringList{u"Tokyo Jihen"_s});
    EXPECT_FALSE(track.hasExtraTag(u"TRACKARTISTLATIN"_s));
}

TEST(SubsonicApiTest, SongsWithoutRomanizationHaveNoExtraTags)
{
    const Track track = Subsonic::songToTrack(
        response(R"({"id": "s1", "title": "Song", "artists": [{"id": "a", "name": "Artist"}], "album": "Album"})"),
        u"subsonic://u@host"_s);

    EXPECT_TRUE(track.extraTags().empty());
}

TEST(SubsonicApiTest, DisplaysNamesAsChosen)
{
    using Subsonic::NameDisplay;

    EXPECT_EQ(Subsonic::displayName(u"東京事変"_s, u"Tokyo Jihen"_s, NameDisplay::Original), u"東京事変"_s);
    EXPECT_EQ(Subsonic::displayName(u"東京事変"_s, u"Tokyo Jihen"_s, NameDisplay::Romanized), u"Tokyo Jihen"_s);
    EXPECT_EQ(Subsonic::displayName(u"東京事変"_s, u"Tokyo Jihen"_s, NameDisplay::Both), u"東京事変 (Tokyo Jihen)"_s);
    // Names without a romanized form fall back to the original
    EXPECT_EQ(Subsonic::displayName(u"Artist"_s, {}, NameDisplay::Romanized), u"Artist"_s);
    EXPECT_EQ(Subsonic::displayName(u"Artist"_s, {}, NameDisplay::Both), u"Artist"_s);
    EXPECT_EQ(Subsonic::displayName({}, u"Latin"_s, NameDisplay::Romanized), QString{});
}
} // namespace Fooyin::Testing

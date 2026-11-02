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

TEST(SubsonicApiTest, KeepsEveryServerFieldForTheProperties)
{
    const Track track = Subsonic::songToTrack(response(R"({
        "id": "s1", "title": "Song", "suffix": "flac", "contentType": "audio/flac", "path": "A/B/01.flac",
        "averageRating": 4.5, "sortName": "Song, The", "displayArtist": "A feat. B", "displayAlbumArtist": "A",
        "displayComposer": "C, D", "moods": ["calm", "warm"], "bpm": 120, "musicBrainzId": "mb-1",
        "isrc": ["US1234567890"], "explicitStatus": "clean", "comment": "Note",
        "contributors": [{"role": "composer", "artist": {"id": "c", "name": "C"}},
                         {"role": "performer", "subRole": "piano", "artist": {"id": "p", "name": "P"}},
                         {"role": "producer", "artist": {"id": "q", "name": "Q"}}],
        "replayGain": {"trackGain": -6.5, "baseGain": 1, "fallbackGain": 0}})"),
                                              u"subsonic://u@host"_s);

    EXPECT_EQ(track.extraTag(u"SUFFIX"_s), QStringList{u"flac"_s});
    EXPECT_EQ(track.extraTag(u"CONTENTTYPE"_s), QStringList{u"audio/flac"_s});
    EXPECT_EQ(track.extraTag(u"SERVERPATH"_s), QStringList{u"A/B/01.flac"_s});
    EXPECT_EQ(track.extraTag(u"AVERAGERATING"_s), QStringList{u"4.5"_s});
    EXPECT_EQ(track.extraTag(u"TITLESORT"_s), QStringList{u"Song, The"_s});
    EXPECT_EQ(track.extraTag(u"DISPLAYARTIST"_s), QStringList{u"A feat. B"_s});
    EXPECT_EQ(track.extraTag(u"MOOD"_s), (QStringList{u"calm"_s, u"warm"_s}));
    EXPECT_EQ(track.extraTag(u"BPM"_s), QStringList{u"120"_s});
    EXPECT_EQ(track.extraTag(u"MUSICBRAINZ_TRACKID"_s), QStringList{u"mb-1"_s});
    EXPECT_EQ(track.extraTag(u"ISRC"_s), QStringList{u"US1234567890"_s});
    EXPECT_EQ(track.extraTag(u"EXPLICITSTATUS"_s), QStringList{u"clean"_s});
    EXPECT_EQ(track.extraTag(u"PRODUCER"_s), QStringList{u"Q"_s});
    EXPECT_EQ(track.extraTag(u"REPLAYGAIN_BASE_GAIN"_s), QStringList{u"1"_s});
    EXPECT_EQ(track.extraTag(u"REPLAYGAIN_FALLBACK_GAIN"_s), QStringList{u"0"_s});
    EXPECT_EQ(track.composers(), QStringList{u"C"_s});
    EXPECT_EQ(track.performers(), QStringList{u"P (piano)"_s});
    EXPECT_EQ(track.comment(), u"Note"_s);
}

TEST(SubsonicApiTest, OmitsFieldsTheServerLeavesOut)
{
    const Track track = Subsonic::songToTrack(
        response(R"({"id": "s1", "title": "Song", "displayComposer": "C, D", "bpm": 0, "explicitStatus": ""})"),
        u"subsonic://u@host"_s);

    EXPECT_TRUE(track.extraTags().empty());
    EXPECT_EQ(track.composers(), QStringList{u"C, D"_s});
}

TEST(SubsonicApiTest, ServerAddressesWithoutASchemeUseHttps)
{
    EXPECT_EQ(QUrl{u"https://music.example.org:4533"_s}, Subsonic::serverUrl(u" music.example.org:4533 "_s));
    EXPECT_EQ(QUrl{u"https://music.example.org/sub"_s}, Subsonic::serverUrl(u"music.example.org/sub"_s));
    EXPECT_EQ(QUrl{u"http://192.168.1.2:4533"_s}, Subsonic::serverUrl(u"http://192.168.1.2:4533"_s));
    EXPECT_TRUE(Subsonic::serverUrl(u""_s).isEmpty());
}
} // namespace Fooyin::Testing

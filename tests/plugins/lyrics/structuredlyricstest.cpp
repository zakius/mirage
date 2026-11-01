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

#include "lyricsparser.h"
#include "sources/structuredlyrics.h"

#include <QJsonDocument>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
QJsonArray structuredLyrics(const char* json)
{
    return QJsonDocument::fromJson(json).array();
}
} // namespace

TEST(StructuredLyricsTest, ConvertsSyncedLinesWithOffset)
{
    const QString text = Lyrics::structuredLyricsToLrc(structuredLyrics(R"([
        {"lang": "xxx", "synced": false, "line": [{"value": "Plain"}]},
        {"lang": "jpn", "synced": true, "offset": 500,
         "line": [{"start": 1134, "value": "夢ならば"}, {"start": 62380, "value": "どれほど"}]}
    ])"));

    EXPECT_EQ(text, u"[offset:500]\n[00:01.13]夢ならば\n[01:02.38]どれほど"_s);

    const auto lyrics = Lyrics::parse(text);
    ASSERT_EQ(lyrics.type, Lyrics::Lyrics::Type::Synced);
    EXPECT_EQ(lyrics.offset, 500);
    ASSERT_EQ(lyrics.lines.size(), 2);
    EXPECT_EQ(lyrics.lines.at(0).joinedWords(), u"夢ならば "_s);
    EXPECT_EQ(lyrics.lines.at(1).timestamp, 62380 - 500);
}

TEST(StructuredLyricsTest, ConvertsUnsyncedLines)
{
    const QString text = Lyrics::structuredLyricsToLrc(structuredLyrics(R"([
        {"lang": "eng", "synced": false, "offset": 100, "line": [{"value": "First"}, {"value": "Second"}]}
    ])"));

    EXPECT_EQ(text, u"First\nSecond"_s);
    EXPECT_EQ(Lyrics::parse(text).type, Lyrics::Lyrics::Type::Unsynced);
}

TEST(StructuredLyricsTest, ReturnsNothingWithoutLines)
{
    EXPECT_TRUE(Lyrics::structuredLyricsToLrc({}).isEmpty());
    EXPECT_TRUE(Lyrics::structuredLyricsToLrc(structuredLyrics(R"([{"synced": true, "line": []}])")).isEmpty());
}
namespace {
// What a server that ignores `kinds` sends: every layer, with word timing
constexpr auto AllLayers = R"([
    {"kind": "main", "lang": "jpn", "synced": true,
     "line": [{"start": 1000, "value": "夢ならば"}, {"start": 3000, "value": "どれほど"}],
     "cueLine": [{"index": 0, "start": 1000, "end": 3000, "value": "夢ならば",
                  "cue": [{"start": 1000, "value": "夢", "byteStart": 0, "byteEnd": 2},
                          {"start": 1500, "value": "ならば", "byteStart": 3, "byteEnd": 11}]}]},
    {"kind": "translation", "lang": "eng", "synced": true,
     "line": [{"start": 1000, "value": "If only"}, {"start": 3000, "value": "How much"}]},
    {"kind": "pronunciation", "lang": "ja-Latn", "synced": true,
     "line": [{"start": 1000, "value": "yume naraba"}, {"start": 3000, "value": "dore hodo"}],
     "cueLine": [{"index": 0, "start": 1000, "end": 3000, "value": "yume naraba",
                  "cue": [{"start": 1000, "value": "yume ", "byteStart": 0, "byteEnd": 4},
                          {"start": 1500, "value": "naraba", "byteStart": 5, "byteEnd": 10}]},
                 {"index": 1, "start": 3000, "value": "dore hodo",
                  "cue": [{"start": 3000, "value": "dore ", "byteStart": 0, "byteEnd": 4},
                          {"start": 3400, "value": "hodo", "byteStart": 5, "byteEnd": 8}]}]}
])";
} // namespace

TEST(StructuredLyricsTest, ShowsMainLyricsOfEnhancedResponses)
{
    EXPECT_EQ(Lyrics::structuredLyricsToLrc(structuredLyrics(AllLayers)),
              u"[00:01.00]<00:01.00>夢<00:01.50>ならば\n[00:03.00]どれほど"_s);
}

TEST(StructuredLyricsTest, ShowsPronunciationWithWordTiming)
{
    const QString text
        = Lyrics::structuredLyricsToLrc(structuredLyrics(AllLayers), Lyrics::LyricsLayers::Pronunciation);
    EXPECT_EQ(text, u"[00:01.00]<00:01.00>yume <00:01.50>naraba\n[00:03.00]<00:03.00>dore <00:03.40>hodo"_s);

    const auto lyrics = Lyrics::parse(text);
    ASSERT_EQ(lyrics.type, Lyrics::Lyrics::Type::SyncedWords);
    ASSERT_EQ(lyrics.lines.size(), 2);
    ASSERT_EQ(lyrics.lines.at(1).words.size(), 2);
    EXPECT_EQ(lyrics.lines.at(1).words.at(1).timestamp, 3400);
}

TEST(StructuredLyricsTest, PairsEachMainLineWithItsPronunciation)
{
    const QString text
        = Lyrics::structuredLyricsToLrc(structuredLyrics(AllLayers), Lyrics::LyricsLayers::MainAndPronunciation);
    EXPECT_EQ(text, u"[00:01.00]<00:01.00>夢<00:01.50>ならば\n[00:01.00]<00:01.00>yume <00:01.50>naraba\n"
                    "[00:03.00]どれほど\n[00:03.00]<00:03.00>dore <00:03.40>hodo"_s);

    const auto lyrics = Lyrics::parse(text);
    ASSERT_EQ(lyrics.lines.size(), 4);
    EXPECT_EQ(lyrics.lines.at(2).joinedWords().trimmed(), u"どれほど"_s);
    EXPECT_EQ(lyrics.lines.at(3).timestamp, 3000);
}

TEST(StructuredLyricsTest, ShowsMainLyricsWithoutPronunciation)
{
    // A Latin-script song: the server answers `kinds=pronunciation` with the main entry
    const auto latinSong = structuredLyrics(R"([
        {"kind": "main", "lang": "eng", "synced": true, "line": [{"start": 1000, "value": "Hello"}]}
    ])");

    EXPECT_EQ(Lyrics::structuredLyricsToLrc(latinSong, Lyrics::LyricsLayers::Pronunciation), u"[00:01.00]Hello"_s);
    EXPECT_EQ(Lyrics::structuredLyricsToLrc(latinSong, Lyrics::LyricsLayers::MainAndPronunciation),
              u"[00:01.00]Hello"_s);
}
} // namespace Fooyin::Testing

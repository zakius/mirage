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
} // namespace Fooyin::Testing

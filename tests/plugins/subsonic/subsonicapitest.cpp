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
} // namespace Fooyin::Testing

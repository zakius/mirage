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

#include <core/scripting/displayname.h>

#include <core/constants.h>
#include <core/track.h>

#include <atomic>

using namespace Qt::StringLiterals;

namespace {
//! Read by script workers.
std::atomic<Fooyin::Scripting::NameDisplay> currentDisplay{Fooyin::Scripting::NameDisplay::Original};
} // namespace

namespace Fooyin::Scripting {
namespace {
QString singleName(const QString& original, const QString& romanized, NameDisplay display)
{
    if(original.isEmpty() || romanized.isEmpty() || romanized == original || display == NameDisplay::Original) {
        return original;
    }
    return display == NameDisplay::Romanized ? romanized : u"%1 (%2)"_s.arg(original, romanized);
}
} // namespace

QString displayName(const QString& original, const QString& romanized, NameDisplay display)
{
    static const QLatin1StringView separator{Constants::UnitSeparator};
    if(!original.contains(separator)) {
        return singleName(original, romanized, display);
    }
    return displayNames(original.split(separator), romanized.split(separator), display).join(separator);
}

QStringList displayNames(const QStringList& originals, const QStringList& romanized, NameDisplay display)
{
    if(romanized.size() != originals.size()) {
        return originals;
    }
    QStringList names;
    names.reserve(originals.size());
    for(qsizetype i{0}; i < originals.size(); ++i) {
        names.append(singleName(originals.at(i), romanized.at(i), display));
    }
    return names;
}

TrackDisplayNames trackDisplayNames(const Track& track)
{
    const NameDisplay display = nameDisplay();
    const auto tag = [&track](const char* name) {
        return track.extraTag(QString::fromLatin1(name));
    };
    return {.title        = singleName(track.effectiveTitle(), tag(TitleLatinTag).value(0), display),
            .album        = singleName(track.album(), tag(AlbumLatinTag).value(0), display),
            .artists      = displayNames(track.artists(), tag(ArtistLatinTag), display),
            .albumArtists = displayNames(track.albumArtists(), tag(AlbumArtistLatinTag), display)};
}

void setNameDisplay(NameDisplay display)
{
    currentDisplay = display;
}

NameDisplay nameDisplay()
{
    return currentDisplay.load();
}
} // namespace Fooyin::Scripting

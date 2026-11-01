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

#include "structuredlyrics.h"

#include "lyricsparser.h"

#include <QJsonObject>
#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Lyrics {
QString structuredLyricsToLrc(const QJsonArray& structuredLyrics)
{
    if(structuredLyrics.isEmpty()) {
        return {};
    }

    const auto synced = std::ranges::find_if(
        structuredLyrics, [](const QJsonValue& entry) { return entry.toObject().value("synced"_L1).toBool(); });
    const QJsonObject entry     = (synced != structuredLyrics.end() ? *synced : structuredLyrics.first()).toObject();
    const bool isSynced         = entry.value("synced"_L1).toBool();
    const QJsonArray entryLines = entry.value("line"_L1).toArray();
    if(entryLines.isEmpty()) {
        return {};
    }

    QStringList lines;
    // Both formats use a positive offset to show lyrics sooner
    if(const qint64 offset = entry.value("offset"_L1).toInteger(); isSynced && offset != 0) {
        lines.append(u"[offset:%1]"_s.arg(offset));
    }
    for(const auto& line : entryLines) {
        const QJsonObject object = line.toObject();
        const QString value      = object.value("value"_L1).toString();
        if(isSynced) {
            const auto start = static_cast<uint64_t>(std::max<qint64>(object.value("start"_L1).toInteger(), 0));
            lines.append(u"[%1]%2"_s.arg(formatTimestamp(start), value));
        }
        else {
            lines.append(value);
        }
    }

    return lines.join(u'\n');
}
} // namespace Fooyin::Lyrics

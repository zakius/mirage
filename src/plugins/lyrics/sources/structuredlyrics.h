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

#pragma once

#include <QJsonArray>
#include <QString>

#include <cstdint>

namespace Fooyin::Lyrics {
//! Lyrics layers of OpenSubsonic `songLyrics` v2 to show.
enum class LyricsLayers : uint8_t
{
    Main,
    Pronunciation,
    MainAndPronunciation,
};

/*!
 * Converts OpenSubsonic `structuredLyrics` to LRC text: the first synced `main` entry as timed lines with its offset,
 * or else the first `main` entry as plain lines. Returns an empty string if there are no lines.
 *
 * With @p layers asking for it, the `pronunciation` entry is shown instead, or after each main line, paired by line
 * index. Its `cueLine` word timing becomes enhanced LRC word timestamps. Without a pronunciation entry, the main
 * lyrics are shown.
 *
 * With @p translation, each line is followed by its line of a `translation` entry: oasis's machine translation into
 * the user's language if there is one, else the first.
 */
[[nodiscard]] QString structuredLyricsToLrc(const QJsonArray& structuredLyrics,
                                            LyricsLayers layers = LyricsLayers::Main, bool translation = false);
} // namespace Fooyin::Lyrics

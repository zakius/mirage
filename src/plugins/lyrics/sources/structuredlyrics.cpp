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
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;

namespace {
//! Returns the first synced entry of @p kind, or else its first entry; entries without a kind are `main`.
QJsonObject entryOfKind(const QJsonArray& structuredLyrics, QLatin1String kind)
{
    QJsonObject first;
    for(const auto& value : structuredLyrics) {
        const QJsonObject entry = value.toObject();
        if(entry.value("kind"_L1).toString(u"main"_s) != kind) {
            continue;
        }
        if(entry.value("synced"_L1).toBool()) {
            return entry;
        }
        if(first.isEmpty()) {
            first = entry;
        }
    }
    return first;
}

//! Returns line @p index of @p entry as LRC, timed at @p start if synced, with the words of its first cueLine.
QString lrcLine(const QJsonObject& entry, qsizetype index, std::optional<uint64_t> start)
{
    const QString value = entry.value("line"_L1).toArray().at(index).toObject().value("value"_L1).toString();
    if(!start) {
        return value;
    }

    const auto timestamp = [](const QJsonValue& time) {
        return Fooyin::Lyrics::formatTimestamp(static_cast<uint64_t>(std::max<qint64>(time.toInteger(), 0)));
    };

    const QJsonArray cueLines = entry.value("cueLine"_L1).toArray();
    // Several cueLines may share an index, one per singer; the first is the main singer's
    const auto cueLine = std::ranges::find_if(
        cueLines, [index](const QJsonValue& line) { return line.toObject().value("index"_L1).toInteger() == index; });
    const QJsonArray cues = cueLine != cueLines.end() ? cueLine->toObject().value("cue"_L1).toArray() : QJsonArray{};

    // Each word runs from its cue to the next cue in the line's text, so text between cues (spaces, romaji merged
    // into the word before) stays; byte offsets that don't fit the line fall back to the cues' values
    const QByteArray utf8 = value.toUtf8();
    std::vector<qsizetype> starts;
    for(const auto& cue : cues) {
        const qsizetype byteStart = cue.toObject().value("byteStart"_L1).toInteger(-1);
        if(byteStart < 0 || byteStart >= utf8.size() || (!starts.empty() && byteStart <= starts.back())) {
            starts.clear();
            break;
        }
        starts.push_back(byteStart);
    }

    QString words;
    for(qsizetype i{0}; i < cues.size(); ++i) {
        const QJsonObject cue = cues.at(i).toObject();
        QString text          = cue.value("value"_L1).toString();
        if(!starts.empty()) {
            const qsizetype end = i + 1 < cues.size() ? starts.at(i + 1) : utf8.size();
            text                = QString::fromUtf8(utf8.sliced(starts.at(i), end - starts.at(i)));
            if(i == 0) {
                words += QString::fromUtf8(utf8.first(starts.front()));
            }
        }
        // The parser drops spaces right after a word's timestamp, so they end the word before
        qsizetype lead{0};
        while(lead < text.size() && text.at(lead).isSpace()) {
            ++lead;
        }
        words += text.first(lead) + u"<%1>"_s.arg(timestamp(cue.value("start"_L1))) + text.sliced(lead);
    }

    return u"[%1]%2"_s.arg(Fooyin::Lyrics::formatTimestamp(*start), words.isEmpty() ? value : words);
}

std::optional<uint64_t> lineStart(const QJsonObject& entry, qsizetype index)
{
    if(!entry.value("synced"_L1).toBool()) {
        return {};
    }
    const qint64 start = entry.value("line"_L1).toArray().at(index).toObject().value("start"_L1).toInteger();
    return static_cast<uint64_t>(std::max<qint64>(start, 0));
}
} // namespace

namespace Fooyin::Lyrics {
QString structuredLyricsToLrc(const QJsonArray& structuredLyrics, LyricsLayers layers, bool translation)
{
    const QJsonObject main          = entryOfKind(structuredLyrics, "main"_L1);
    const QJsonObject pronunciation = entryOfKind(structuredLyrics, "pronunciation"_L1);
    // Without a pronunciation entry, as for a Latin-script song, the main lyrics are shown
    const QJsonObject entry
        = main.isEmpty() || (layers == LyricsLayers::Pronunciation && !pronunciation.isEmpty()) ? pronunciation : main;
    const QJsonObject paired
        = layers == LyricsLayers::MainAndPronunciation && !main.isEmpty() ? pronunciation : QJsonObject{};

    const QJsonArray entryLines = entry.value("line"_L1).toArray();
    if(entryLines.isEmpty()) {
        return {};
    }

    QStringList lines;
    // Both formats use a positive offset to show lyrics sooner
    if(const qint64 offset = entry.value("offset"_L1).toInteger(); entry.value("synced"_L1).toBool() && offset != 0) {
        lines.append(u"[offset:%1]"_s.arg(offset));
    }
    // Pronunciation lines match the main lines one to one, so each follows its main line at the same time. A line
    // already in Latin script, such as an English line between translated ones, reads the same and isn't repeated.
    const QJsonArray pairedLines = paired.value("line"_L1).toArray();
    const auto text              = [](const QJsonArray& lines, qsizetype i) {
        return lines.at(i).toObject().value("value"_L1).toString().simplified();
    };
    // Translations have one line per main line, empty where a line has none
    QJsonArray translationLines;
    if(translation) {
        for(const auto& value : structuredLyrics) {
            const QJsonObject candidate = value.toObject();
            if(candidate.value("kind"_L1).toString() == "translation"_L1
               && (translationLines.isEmpty() || candidate.value("oasisMachineTranslation"_L1).toBool())) {
                translationLines = candidate.value("line"_L1).toArray();
            }
        }
    }
    for(qsizetype i{0}; i < entryLines.size(); ++i) {
        const auto start = lineStart(entry, i);
        lines.append(lrcLine(entry, i, start));
        if(i < pairedLines.size() && text(pairedLines, i).compare(text(entryLines, i), Qt::CaseInsensitive) != 0) {
            lines.append(lrcLine(paired, i, start));
        }
        if(const QString translated = i < translationLines.size() ? text(translationLines, i) : QString{};
           !translated.isEmpty() && translated.compare(text(entryLines, i), Qt::CaseInsensitive) != 0) {
            lines.append(start ? u"[%1]%2"_s.arg(formatTimestamp(*start), translated) : translated);
        }
    }

    return lines.join(u'\n');
}
} // namespace Fooyin::Lyrics

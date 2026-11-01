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

#include "subsoniclyrics.h"

#include "structuredlyrics.h"

#include <core/library/libraryprovider.h>
#include <core/network/networkaccessmanager.h>
#include <core/network/networkutils.h>
#include <core/scripting/displayname.h>
#include <utils/settings/settingsmanager.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>

#include <algorithm>

using namespace Qt::StringLiterals;

constexpr auto SongMarker = "/song/";
//! The Subsonic plugin's name display setting: 0 original, 1 romanized, 2 both.
constexpr auto NameDisplaySetting = "Subsonic/NameDisplay";

namespace {
//! Servers may send a single object in place of a one-element array.
QJsonArray toArray(const QJsonValue& value)
{
    return value.isArray() ? value.toArray() : (value.isObject() ? QJsonArray{value} : QJsonArray{});
}
} // namespace

namespace Fooyin::Lyrics {
SubsonicLyrics::SubsonicLyrics(NetworkAccessManager* network, SettingsManager* settings,
                               std::shared_ptr<LibraryProviderRegistry> providers, int index, bool enabled,
                               QObject* parent)
    : LyricSource{network, settings, index, enabled, parent}
    , m_providers{std::move(providers)}
{ }

QString SubsonicLyrics::name() const
{
    return u"Subsonic"_s;
}

bool SubsonicLyrics::isLocal() const
{
    return true;
}

void SubsonicLyrics::search(const SearchParams& params)
{
    cancel();

    const QString path  = params.track.filepath();
    const auto index    = path.lastIndexOf(QLatin1String{SongMarker});
    const auto provider = m_providers ? m_providers->providerForPath(path) : nullptr;
    if(!provider || provider->scheme() != "subsonic"_L1 || index < 0) {
        Q_EMIT searchResult({});
        return;
    }

    m_params = params;
    m_server = path.left(index);
    m_songId = QUrl::fromPercentEncoding(path.sliced(index + QLatin1String{SongMarker}.size()).toUtf8());

    if(m_extensions.contains(m_server)) {
        requestLyrics();
        return;
    }

    get(u"getOpenSubsonicExtensions"_s, {}, [this](const QJsonObject& response) {
        const QJsonArray extensions = toArray(response.value("openSubsonicExtensions"_L1));
        const auto has              = [&extensions](QLatin1String name) {
            return std::ranges::any_of(extensions, [name](const QJsonValue& extension) {
                return extension.toObject().value("name"_L1) == name;
            });
        };
        // Servers without OpenSubsonic answer with an error; a failed request is asked again next time
        if(!response.isEmpty()) {
            m_extensions[m_server] = {.songLyrics = has("songLyrics"_L1), .romanization = has("romanization"_L1)};
        }
        requestLyrics();
    });
}

void SubsonicLyrics::get(const QString& method, const QUrlQuery& params,
                         const std::function<void(const QJsonObject&)>& handler)
{
    QUrl url{m_server + u"/rest/"_s + method};
    url.setQuery(params);

    const auto provider = m_providers->providerForPath(m_server);
    const QUrl resolved = provider ? provider->resolveUrl(url) : QUrl{};
    if(!resolved.isValid()) {
        Q_EMIT searchResult({});
        return;
    }

    setReply(network()->get(makeNetworkRequest(resolved)));
    QObject::connect(reply(), &QNetworkReply::finished, this, [this, handler]() {
        auto* lyricsReply = reply();
        if(!lyricsReply) {
            return;
        }

        QJsonObject obj;
        const bool success = getJsonFromReply(lyricsReply, &obj);
        resetReply();

        handler(success ? obj.value("subsonic-response"_L1).toObject() : QJsonObject{});
    });
}

void SubsonicLyrics::requestLyrics()
{
    const Extensions extensions = m_extensions.contains(m_server) ? m_extensions.at(m_server) : Extensions{};

    QUrlQuery params;
    if(extensions.songLyrics) {
        params.addQueryItem(u"id"_s, m_songId);

        auto layers = LyricsLayers::Main;
        if(extensions.romanization) {
            const auto display = static_cast<Scripting::NameDisplay>(settings()->value(NameDisplaySetting).toInt());
            layers = display == Scripting::NameDisplay::Romanized ? LyricsLayers::Pronunciation
                   : display == Scripting::NameDisplay::Both      ? LyricsLayers::MainAndPronunciation
                                                                  : LyricsLayers::Main;
        }
        // Servers that ignore `kinds` send every layer; the conversion picks the ones asked for
        if(layers != LyricsLayers::Main) {
            params.addQueryItem(u"enhanced"_s, u"true"_s);
            params.addQueryItem(u"kinds"_s,
                                layers == LyricsLayers::Pronunciation ? u"pronunciation"_s : u"main,pronunciation"_s);
        }

        get(u"getLyricsBySongId"_s, params, [this, layers](const QJsonObject& response) {
            LyricData data;
            // Lyrics found by song id need no title or artist match
            data.data = structuredLyricsToLrc(
                toArray(response.value("lyricsList"_L1).toObject().value("structuredLyrics"_L1)), layers);
            Q_EMIT searchResult(data.data.isEmpty() ? std::vector<LyricData>{} : std::vector<LyricData>{data});
        });
        return;
    }

    params.addQueryItem(u"artist"_s, m_params.artist);
    params.addQueryItem(u"title"_s, m_params.title);
    get(u"getLyrics"_s, params, [this](const QJsonObject& response) {
        const QJsonObject lyrics = response.value("lyrics"_L1).toObject();

        LyricData data;
        data.data   = lyrics.value("value"_L1).toString();
        data.artist = lyrics.value("artist"_L1).toString();
        data.title  = lyrics.value("title"_L1).toString();
        Q_EMIT searchResult(data.data.isEmpty() ? std::vector<LyricData>{} : std::vector<LyricData>{data});
    });
}
} // namespace Fooyin::Lyrics

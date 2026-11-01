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

#include <core/network/networkutils.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>

using namespace Qt::StringLiterals;

constexpr auto ClientName = "mirage";
constexpr auto ApiVersion = "1.16.1";

namespace {
QString randomSalt()
{
    return QString::number(QRandomGenerator::global()->generate64(), 16);
}

uint64_t toMSecs(const QJsonValue& value)
{
    const auto time = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    return time.isValid() ? static_cast<uint64_t>(time.toMSecsSinceEpoch()) : 0;
}

QStringList names(const QJsonValue& value)
{
    QStringList result;
    for(const auto& item : Fooyin::Subsonic::toArray(value)) {
        if(const QString name = item.toObject().value("name"_L1).toString(); !name.isEmpty()) {
            result.append(name);
        }
    }
    return result;
}

//! Returns the romanized names of a list of artists, keeping names without one, or nothing if none has one.
QStringList latinNames(const QJsonValue& value)
{
    QStringList result;
    bool romanized{false};
    for(const auto& item : Fooyin::Subsonic::toArray(value)) {
        const QJsonObject object = item.toObject();
        const QString name       = object.value("name"_L1).toString();
        const QString latin      = object.value("nameLatin"_L1).toString();
        if(!name.isEmpty()) {
            romanized |= !latin.isEmpty();
            result.append(latin.isEmpty() ? name : latin);
        }
    }
    return romanized ? result : QStringList{};
}

void setLatinTag(Fooyin::Track& track, const char* tag, const QStringList& values)
{
    if(!values.isEmpty() && !values.constFirst().isEmpty()) {
        track.replaceExtraTag(QString::fromLatin1(tag), values);
    }
}

QString codecForSuffix(const QString& suffix)
{
    static const std::map<QString, QString> codecs{
        {u"mp3"_s, u"MP3"_s},   {u"flac"_s, u"FLAC"_s}, {u"ogg"_s, u"Vorbis"_s}, {u"oga"_s, u"Vorbis"_s},
        {u"opus"_s, u"Opus"_s}, {u"m4a"_s, u"AAC"_s},   {u"aac"_s, u"AAC"_s},    {u"wav"_s, u"PCM"_s},
    };
    const auto it = codecs.find(suffix.toLower());
    return it != codecs.cend() ? it->second : suffix.toUpper();
}

QString idFromPath(const QString& path, const QString& kind)
{
    const QString marker = u"/"_s + kind + u"/"_s;
    const auto index     = path.lastIndexOf(marker);
    return index < 0 ? QString{} : QUrl::fromPercentEncoding(path.sliced(index + marker.size()).toUtf8());
}
} // namespace

namespace Fooyin::Subsonic {
QUrl apiUrl(const Account& account, const QString& method, QUrlQuery params)
{
    const QString salt  = randomSalt();
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash((account.password + salt).toUtf8(), QCryptographicHash::Md5).toHex());

    params.addQueryItem(u"u"_s, account.username);
    params.addQueryItem(u"t"_s, token);
    params.addQueryItem(u"s"_s, salt);
    params.addQueryItem(u"v"_s, QString::fromLatin1(ApiVersion));
    params.addQueryItem(u"c"_s, QString::fromLatin1(ClientName));
    params.addQueryItem(u"f"_s, u"json"_s);

    QUrl url{account.url};
    QString path = url.path();
    if(!path.endsWith('/'_L1)) {
        path.append('/'_L1);
    }
    url.setPath(path + u"rest/"_s + method);
    url.setQuery(params);
    return url;
}

void request(QNetworkAccessManager* network, const Account& account, const QString& method, const QUrlQuery& params,
             QObject* context, ResponseCallback callback)
{
    auto* reply = network->get(makeNetworkRequest(apiUrl(account, method, params)));
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(callback)]() {
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError) {
            callback({}, {.message = replyError(reply)});
            return;
        }

        const QJsonObject response
            = QJsonDocument::fromJson(reply->readAll()).object().value("subsonic-response"_L1).toObject();
        if(response.value("status"_L1).toString() != "ok"_L1) {
            const QJsonObject error = response.value("error"_L1).toObject();
            const QString message   = error.value("message"_L1).toString();
            callback({}, {.code    = error.value("code"_L1).toInt(-1),
                          .message = message.isEmpty() ? u"Invalid server response"_s : message});
            return;
        }

        callback(response, {});
    });
}

QString replyError(const QNetworkReply* reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(status > 0) {
        return u"HTTP %1 %2"_s.arg(status).arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString());
    }
    return QString::fromLatin1(QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(reply->error()));
}

QJsonArray toArray(const QJsonValue& value)
{
    if(value.isArray()) {
        return value.toArray();
    }
    if(value.isObject()) {
        return QJsonArray{value};
    }
    return {};
}

Track songToTrack(const QJsonObject& song, const QString& libraryPath)
{
    const QString id = song.value("id"_L1).toString();
    Track track{libraryPath + u"/song/"_s + QString::fromLatin1(QUrl::toPercentEncoding(id))};

    track.setTitle(song.value("title"_L1).toString());
    track.setAlbum(song.value("album"_L1).toString());

    setLatinTag(track, TitleLatinTag, {song.value("titleLatin"_L1).toString()});
    setLatinTag(track, AlbumLatinTag, {song.value("albumLatin"_L1).toString()});

    QStringList artists = names(song.value("artists"_L1));
    if(artists.isEmpty() && song.contains("artist"_L1)) {
        artists.append(song.value("artist"_L1).toString());
        setLatinTag(track, ArtistLatinTag, {song.value("artistLatin"_L1).toString()});
    }
    else {
        setLatinTag(track, ArtistLatinTag, latinNames(song.value("artists"_L1)));
    }
    track.setArtists(artists);

    QStringList albumArtists = names(song.value("albumArtists"_L1));
    if(albumArtists.isEmpty() && song.contains("displayAlbumArtist"_L1)) {
        albumArtists.append(song.value("displayAlbumArtist"_L1).toString());
        setLatinTag(track, AlbumArtistLatinTag, {song.value("displayAlbumArtistLatin"_L1).toString()});
    }
    else {
        setLatinTag(track, AlbumArtistLatinTag, latinNames(song.value("albumArtists"_L1)));
    }
    track.setAlbumArtists(albumArtists);

    if(const QStringList artistsLatin = track.extraTag(QString::fromLatin1(ArtistLatinTag));
       !albumArtists.isEmpty() && artistsLatin.size() == artists.size()) {
        QStringList trackArtistsLatin;
        for(qsizetype i{0}; i < artists.size(); ++i) {
            if(!albumArtists.contains(artists.at(i))) {
                trackArtistsLatin.append(artistsLatin.at(i));
            }
        }
        if(trackArtistsLatin != track.uniqueArtists()) {
            setLatinTag(track, TrackArtistLatinTag, trackArtistsLatin);
        }
    }

    QStringList genres = names(song.value("genres"_L1));
    if(genres.isEmpty() && song.contains("genre"_L1)) {
        genres.append(song.value("genre"_L1).toString());
    }
    track.setGenres(genres);

    if(const int number = song.value("track"_L1).toInt(); number > 0) {
        track.setTrackNumber(QString::number(number));
    }
    if(const int disc = song.value("discNumber"_L1).toInt(); disc > 0) {
        track.setDiscNumber(QString::number(disc));
    }
    if(const int year = song.value("year"_L1).toInt(); year > 0) {
        track.setDate(QString::number(year));
    }
    track.setComment(song.value("comment"_L1).toString());

    const QString suffix = song.value("suffix"_L1).toString();
    if(!suffix.isEmpty()) {
        track.setCodec(codecForSuffix(suffix));
    }
    track.setDuration(static_cast<uint64_t>(song.value("duration"_L1).toDouble() * 1000));
    track.setFileSize(static_cast<uint64_t>(song.value("size"_L1).toInteger()));
    track.setBitrate(song.value("bitRate"_L1).toInt());
    track.setSampleRate(song.value("samplingRate"_L1).toInt());
    track.setChannels(song.value("channelCount"_L1).toInt());
    track.setBitDepth(song.value("bitDepth"_L1).toInt());

    const QJsonObject replayGain = song.value("replayGain"_L1).toObject();
    if(replayGain.contains("trackGain"_L1)) {
        track.setRGTrackGain(static_cast<float>(replayGain.value("trackGain"_L1).toDouble()));
    }
    if(replayGain.contains("albumGain"_L1)) {
        track.setRGAlbumGain(static_cast<float>(replayGain.value("albumGain"_L1).toDouble()));
    }
    if(replayGain.contains("trackPeak"_L1)) {
        track.setRGTrackPeak(static_cast<float>(replayGain.value("trackPeak"_L1).toDouble()));
    }
    if(replayGain.contains("albumPeak"_L1)) {
        track.setRGAlbumPeak(static_cast<float>(replayGain.value("albumPeak"_L1).toDouble()));
    }

    track.setAddedTime(toMSecs(song.value("created"_L1)));
    track.setLastPlayed(toMSecs(song.value("played"_L1)));
    track.setPlayCount(song.value("playCount"_L1).toInt());
    track.setRatingStars(song.value("userRating"_L1).toInt() * 2);
    track.setLoved(song.contains("starred"_L1));

    if(const QString cover = song.value("coverArt"_L1).toString(); !cover.isEmpty()) {
        track.replaceExtraTag(u"ARTWORKURL"_s,
                              libraryPath + u"/cover/"_s + QString::fromLatin1(QUrl::toPercentEncoding(cover)));
    }

    return track;
}

std::optional<bool> serverScrobbles(const QJsonObject& getUserResponse)
{
    const QJsonValue enabled = getUserResponse.value("user"_L1).toObject().value("scrobblingEnabled"_L1);
    return enabled.isBool() ? std::optional{enabled.toBool()} : std::nullopt;
}

bool scrobblesLocally(std::optional<bool> choice, std::optional<bool> serverScrobbles)
{
    return choice.value_or(serverScrobbles != true);
}

bool hasExtension(const QJsonObject& response, const QString& name, int version)
{
    return std::ranges::any_of(toArray(response.value("openSubsonicExtensions"_L1)), [&](const QJsonValue& value) {
        const QJsonObject extension = value.toObject();
        return extension.value("name"_L1).toString() == name
            && toArray(extension.value("versions"_L1)).contains(QJsonValue{version});
    });
}

QString songId(const QString& trackPath)
{
    return idFromPath(trackPath, u"song"_s);
}

QString coverId(const QString& artworkPath)
{
    return idFromPath(artworkPath, u"cover"_s);
}
} // namespace Fooyin::Subsonic

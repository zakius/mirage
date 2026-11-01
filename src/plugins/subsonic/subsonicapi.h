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

#include <core/track.h>

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QObject;

namespace Fooyin::Subsonic {
//! Extra tags holding the romanized names of the OpenSubsonic `romanization` extension, where they differ.
constexpr auto TitleLatinTag       = "TITLELATIN";
constexpr auto AlbumLatinTag       = "ALBUMLATIN";
constexpr auto ArtistLatinTag      = "ARTISTLATIN";
//! The romanized names of the artists that are not album artists, as %trackartist% lists them.
constexpr auto TrackArtistLatinTag = "TRACKARTISTLATIN";
constexpr auto AlbumArtistLatinTag = "ALBUMARTISTLATIN";

struct Account
{
    QUrl url;
    QString username;
    QString password;
    //! Whether mirage's scrobbler also scrobbles this server's plays; std::nullopt follows the server.
    std::optional<bool> scrobble;
};

struct ApiError
{
    //! Subsonic error code, or -1 for network and protocol errors.
    int code{-1};
    QString message;
};

//! Receives the "subsonic-response" object of a successful call, or std::nullopt and the error.
using ResponseCallback = std::function<void(std::optional<QJsonObject> response, ApiError error)>;

//! Returns the authenticated URL of the API @p method. Each call uses a new salt.
[[nodiscard]] QUrl apiUrl(const Account& account, const QString& method, QUrlQuery params = {});

//! Calls the API @p method and invokes @p callback in the thread of @p context.
QNetworkReply* request(QNetworkAccessManager* network, const Account& account, const QString& method,
                       const QUrlQuery& params, QObject* context, ResponseCallback callback);
//! POSTs @p body to the API @p method, with the authentication in the query, like request().
QNetworkReply* post(QNetworkAccessManager* network, const Account& account, const QString& method,
                    const QUrlQuery& params, const QByteArray& body, QObject* context, ResponseCallback callback);

//! Describes a failed reply without its URL, which contains the authentication token.
[[nodiscard]] QString replyError(const QNetworkReply* reply);

//! Returns a JSON value as an array; servers may send a single object in place of a one-element array.
[[nodiscard]] QJsonArray toArray(const QJsonValue& value);

/*!
 * Converts a Subsonic song to a track with the path `<libraryPath>/song/<id>`.
 * Only fields stored in the database are set. Fields without a track field (romanized names, sort name, moods, ISRC,
 * contributor roles and others) go to extra tags, so the properties view shows everything the server sends.
 */
[[nodiscard]] Track songToTrack(const QJsonObject& song, const QString& libraryPath);

//! Returns getUser's `scrobblingEnabled`: whether the server scrobbles the user's plays itself, if it says.
[[nodiscard]] std::optional<bool> serverScrobbles(const QJsonObject& getUserResponse);
/*!
 * Returns whether mirage's scrobbler also scrobbles a server's plays: the user's @p choice, else on unless the server
 * scrobbles them itself.
 */
[[nodiscard]] bool scrobblesLocally(std::optional<bool> choice, std::optional<bool> serverScrobbles);

//! Returns getUser's `uploadRole`: whether the user may upload.
[[nodiscard]] bool uploadRole(const QJsonObject& getUserResponse);

//! True if a getOpenSubsonicExtensions response lists version @p version of the extension @p name.
[[nodiscard]] bool hasExtension(const QJsonObject& response, const QString& name, int version);

//! Returns the song id of a track path, or an empty string.
[[nodiscard]] QString songId(const QString& trackPath);
//! Returns the cover id of an artwork path, or an empty string.
[[nodiscard]] QString coverId(const QString& artworkPath);
} // namespace Fooyin::Subsonic

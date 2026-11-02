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

#include <core/scripting/displayname.h>
#include <core/track.h>
#include <utils/mirage.h>

#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QObject;

namespace Fooyin::Subsonic {
// The OpenSubsonic `romanization` extension's names go to the core's romanized name tags
using Scripting::AlbumArtistLatinTag;
using Scripting::AlbumLatinTag;
using Scripting::ArtistLatinTag;
using Scripting::TitleLatinTag;
using Scripting::TrackArtistLatinTag;

using Mirage::AlbumArtistIdTag;
using Mirage::AlbumIdTag;
using Mirage::AlbumStarredTag;
using Mirage::ArtistIdTag;
using Mirage::ArtistStarredTag;

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
    //! The error object's `duplicate` flag: commitUpload's "already in the library" (extension `upload`).
    bool duplicate{false};
};

//! Receives the "subsonic-response" object of a successful call, or std::nullopt and the error.
using ResponseCallback = std::function<void(std::optional<QJsonObject> response, ApiError error)>;

//! Returns the server URL the user typed in @p address, with https for an address without a scheme.
[[nodiscard]] QUrl serverUrl(const QString& address);

//! Returns the authenticated URL of the API @p method. Each call uses a new salt.
[[nodiscard]] QUrl apiUrl(const Account& account, const QString& method, QUrlQuery params = {});

//! Returns a request for an API URL, which carries the credentials (NetworkRequestOption::Credentials).
[[nodiscard]] QNetworkRequest apiRequest(const QUrl& url);

//! Calls the API @p method and invokes @p callback in the thread of @p context.
QNetworkReply* request(QNetworkAccessManager* network, const Account& account, const QString& method,
                       const QUrlQuery& params, QObject* context, ResponseCallback callback);
//! Calls the API @p method like request(), with the parameters and authentication as a form in the body (OpenSubsonic
//! extension `formPost`), so they can be longer than a URL allows.
QNetworkReply* postForm(QNetworkAccessManager* network, const Account& account, const QString& method,
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

//! Returns the server ids of a track's album artists, or of its artist where the server names no album artists.
[[nodiscard]] QStringList artistIds(const Track& track);
//! Sets or removes the starred extra tag @p tag of @p track.
void setStarred(Track& track, const char* tag, bool starred);
//! Sets the album and artist starred tags of @p tracks from a getStarred2 response.
void markStarred(TrackList& tracks, const QJsonObject& getStarred2Response);

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

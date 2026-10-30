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
class QObject;

namespace Fooyin::Subsonic {
struct Account
{
    QUrl url;
    QString username;
    QString password;
};

//! Receives the "subsonic-response" object of a successful call, or std::nullopt and an error message.
using ResponseCallback = std::function<void(std::optional<QJsonObject> response, QString error)>;

//! Returns the authenticated URL of the API @p method. Each call uses a new salt.
[[nodiscard]] QUrl apiUrl(const Account& account, const QString& method, QUrlQuery params = {});

//! Calls the API @p method and invokes @p callback in the thread of @p context.
void request(QNetworkAccessManager* network, const Account& account, const QString& method, const QUrlQuery& params,
             QObject* context, ResponseCallback callback);

//! Returns a JSON value as an array; servers may send a single object in place of a one-element array.
[[nodiscard]] QJsonArray toArray(const QJsonValue& value);

/*!
 * Converts a Subsonic song to a track with the path `<libraryPath>/song/<id>`.
 * Only fields stored in the database are set.
 */
[[nodiscard]] Track songToTrack(const QJsonObject& song, const QString& libraryPath);

//! Returns the song id of a track path, or an empty string.
[[nodiscard]] QString songId(const QString& trackPath);
//! Returns the cover id of an artwork path, or an empty string.
[[nodiscard]] QString coverId(const QString& artworkPath);
} // namespace Fooyin::Subsonic

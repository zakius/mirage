/*
 * Fooyin
 * Copyright © 2026, Luke Taylor <luket@pm.me>
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

#include "fycore_export.h"

#include <QByteArray>
#include <QFlags>
#include <QNetworkRequest>

class QUrl;

namespace Fooyin {
enum class NetworkRequestOption : uint8_t
{
    AlwaysNetwork         = 1 << 0,
    AcceptAny             = 1 << 1,
    IcyMetadata           = 1 << 2,
    UserVerifiedRedirects = 1 << 3,
    //! The URL carries credentials: the reply never goes to the disk cache, and a NetworkAccessManager follows
    //! redirects only to the same host, and from http to https at most, so the credentials never reach another host.
    Credentials = 1 << 4,
};
Q_DECLARE_FLAGS(NetworkRequestOptions, NetworkRequestOption)

[[nodiscard]] FYCORE_EXPORT QByteArray networkUserAgent();
[[nodiscard]] FYCORE_EXPORT bool isHlsStreamUrl(const QUrl& url);
[[nodiscard]] FYCORE_EXPORT QNetworkRequest makeNetworkRequest(const QUrl& url, NetworkRequestOptions options = {});

//! Set on requests made with NetworkRequestOption::Credentials.
constexpr auto CredentialsAttribute = static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 1);
} // namespace Fooyin

Q_DECLARE_OPERATORS_FOR_FLAGS(Fooyin::NetworkRequestOptions)

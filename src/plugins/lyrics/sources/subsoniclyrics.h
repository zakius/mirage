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

#include "lyricsource.h"

#include <functional>
#include <map>
#include <memory>

namespace Fooyin {
class LibraryProviderRegistry;

namespace Lyrics {
/*!
 * Lyrics from the Subsonic server of a track: OpenSubsonic `getLyricsBySongId` if the server lists the `songLyrics`
 * extension, else `getLyrics` by artist and title. Requests are signed through the provider's resolveUrl().
 *
 * The server holds a track's own lyrics, so they count as local: they are searched without automatic searching, rank
 * after lyrics files and tags, and are not saved back.
 */
class SubsonicLyrics : public LyricSource
{
    Q_OBJECT

public:
    SubsonicLyrics(NetworkAccessManager* network, SettingsManager* settings,
                   std::shared_ptr<LibraryProviderRegistry> providers, int index, bool enabled,
                   QObject* parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] bool isLocal() const override;
    void search(const SearchParams& params) override;

private:
    void get(const QString& method, const QUrlQuery& params, const std::function<void(const QJsonObject&)>& handler);
    void requestLyrics(bool bySongId);

    std::shared_ptr<LibraryProviderRegistry> m_providers;
    //! Whether each server, by library path, lists the songLyrics extension.
    std::map<QString, bool> m_songLyrics;

    SearchParams m_params;
    QString m_server;
    QString m_songId;
};
} // namespace Lyrics
} // namespace Fooyin

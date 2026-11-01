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

#include "fycore_export.h"

#include <core/library/libraryinfo.h>
#include <core/network/remotesourceprovider.h>
#include <core/track.h>

#include <QUrl>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>

namespace Fooyin {
/*!
 * A source of library tracks that are not files, such as a music server.
 *
 * A provider library is a LibraryInfo whose path is a URL with the provider's scheme. Its tracks use URLs under that
 * path. Scanning such a library calls fetchTracks() instead of reading the filesystem, and playback opens tracks
 * through createStreamSource().
 */
class FYCORE_EXPORT LibraryProvider
{
public:
    /** Receives the complete track list of a library, or std::nullopt if it could not be fetched. */
    using FetchCallback = std::function<void(std::optional<TrackList> tracks)>;

    virtual ~LibraryProvider() = default;

    /** Returns the URL scheme owned by this provider, without the trailing colon. */
    [[nodiscard]] virtual QString scheme() const = 0;

    /*!
     * Fetches all tracks of @p library. Called on the main thread; @p callback must also be called on the main thread.
     * Tracks should only set fields stored in the database, so that an unchanged track compares equal on the next sync.
     */
    virtual void fetchTracks(const LibraryInfo& library, FetchCallback callback) = 0;

    /** Opens the track at @p url for playback. Called from engine threads. */
    [[nodiscard]] virtual RemoteStreamSource createStreamSource(const QUrl& url) const = 0;

    /** Maps a provider URL, such as an artwork URL, to a fetchable HTTP(S) URL. Called from any thread. */
    [[nodiscard]] virtual QUrl resolveUrl(const QUrl& url) const = 0;

    /** Returns @c true if the provider reports plays of @p track itself, so client-side scrobblers skip it. */
    [[nodiscard]] virtual bool reportsPlays(const Track& track) const;

    /*!
     * Searches @p library on its server for @p query, so a library search can show tracks the library does not hold.
     * Called on the main thread; @p callback must also be called on the main thread. The default finds nothing.
     */
    virtual void searchTracks(const LibraryInfo& library, const QString& query, FetchCallback callback);
};

/*!
 * Thread-safe registry of library providers, keyed by URL scheme.
 */
class FYCORE_EXPORT LibraryProviderRegistry
{
public:
    void addProvider(const std::shared_ptr<LibraryProvider>& provider);

    /** Returns the provider for @p scheme, or nullptr. */
    [[nodiscard]] std::shared_ptr<LibraryProvider> provider(const QString& scheme) const;
    /** Returns the provider owning @p path (a library or track path), or nullptr for local paths. */
    [[nodiscard]] std::shared_ptr<LibraryProvider> providerForPath(const QString& path) const;

private:
    mutable std::shared_mutex m_mutex;
    std::map<QString, std::shared_ptr<LibraryProvider>> m_providers;
};
} // namespace Fooyin

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

#include <core/library/libraryprovider.h>

#include <mutex>

namespace Fooyin {
bool LibraryProvider::reportsPlays() const
{
    return false;
}

void LibraryProvider::searchTracks(const LibraryInfo& /*library*/, const QString& /*query*/, FetchCallback callback)
{
    callback({});
}

void LibraryProviderRegistry::addProvider(const std::shared_ptr<LibraryProvider>& provider)
{
    const std::unique_lock lock{m_mutex};
    m_providers[provider->scheme().toLower()] = provider;
}

std::shared_ptr<LibraryProvider> LibraryProviderRegistry::provider(const QString& scheme) const
{
    const std::shared_lock lock{m_mutex};
    const auto it = m_providers.find(scheme.toLower());
    return it != m_providers.cend() ? it->second : nullptr;
}

std::shared_ptr<LibraryProvider> LibraryProviderRegistry::providerForPath(const QString& path) const
{
    if(!Track::isVirtualPath(path)) {
        return nullptr;
    }
    return provider(QUrl{path}.scheme());
}
} // namespace Fooyin

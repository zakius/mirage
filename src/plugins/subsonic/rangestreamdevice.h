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

#include <QIODevice>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;

namespace Fooyin::Subsonic {
struct RangeStreamState;

/*!
 * A seekable read-only device for an HTTP resource. It fetches the resource in chunks with range requests and
 * prefetches the next chunks, retrying failed requests. If the server ignores range requests, the whole resource is
 * downloaded once and read while it arrives.
 * Reads block until data arrives, so the device must not be read from the thread of the network manager.
 */
class RangeStreamDevice : public QIODevice
{
    Q_OBJECT

public:
    RangeStreamDevice(std::shared_ptr<QNetworkAccessManager> network, QUrl url, QObject* parent = nullptr);
    ~RangeStreamDevice() override;

    bool open(OpenMode mode) override;
    void close() override;

    [[nodiscard]] bool isSequential() const override;
    [[nodiscard]] qint64 size() const override;
    [[nodiscard]] bool atEnd() const override;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    std::shared_ptr<QNetworkAccessManager> m_network;
    QUrl m_url;
    std::shared_ptr<RangeStreamState> m_state;
};
} // namespace Fooyin::Subsonic

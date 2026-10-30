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

#include "rangestreamdevice.h"

#include "subsonicapi.h"

#include <core/network/networkutils.h>

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

Q_LOGGING_CATEGORY(SUBSONIC_STREAM, "fy.subsonic.stream")

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

constexpr qint64 ChunkSize       = 1024 * 1024;
constexpr qint64 ChunksAhead     = 2;
constexpr int MaxAttempts        = 3;
constexpr auto RetryDelay        = 1s;
constexpr auto RequestTimeout    = 30s;
constexpr int OkCode             = 200;
constexpr int PartialContentCode = 206;
constexpr auto WholeBodyProperty = "fyWholeBody";

namespace Fooyin::Subsonic {
struct RangeStreamState
{
    std::mutex mutex;
    std::condition_variable ready;

    std::map<qint64, QByteArray> chunks;
    std::set<qint64> pending;
    std::map<qint64, int> attempts;
    //! Total size, or -1 while unknown.
    qint64 size{-1};
    QString error;
    bool closed{false};

    //! Set when the server answered a range request with the whole resource, which is read while it downloads.
    bool wholeBody{false};
    bool bodyComplete{false};
    QByteArray body;
};

namespace {
qint64 totalFromContentRange(const QByteArray& header)
{
    static const QRegularExpression regex{uR"(/(\d+)\s*$)"_s};
    const auto match = regex.match(QString::fromLatin1(header));
    return match.hasMatch() ? match.captured(1).toLongLong() : -1;
}

bool isWholeBody(const QNetworkReply* reply)
{
    return reply->property(WholeBodyProperty).toBool();
}

// Runs in the network thread
void startChunkRequest(const std::shared_ptr<QNetworkAccessManager>& network, const QUrl& url,
                       const std::shared_ptr<RangeStreamState>& state, qint64 chunk)
{
    QNetworkRequest request = makeNetworkRequest(url);
    request.setRawHeader("Range", "bytes=" + QByteArray::number(chunk * ChunkSize) + "-"
                                      + QByteArray::number(((chunk + 1) * ChunkSize) - 1));
    request.setTransferTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(RequestTimeout));

    auto* reply = network->get(request);

    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [state, reply]() {
        if(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != OkCode) {
            return;
        }

        const std::scoped_lock lock{state->mutex};
        if(state->wholeBody) {
            return;
        }
        bool ok{false};
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
        state->wholeBody    = true;
        state->size         = ok ? length : -1;
        state->chunks.clear();
        reply->setProperty(WholeBodyProperty, true);
    });
    QObject::connect(reply, &QIODevice::readyRead, reply, [state, reply]() {
        if(!isWholeBody(reply)) {
            return;
        }
        {
            const std::scoped_lock lock{state->mutex};
            state->body.append(reply->readAll());
        }
        state->ready.notify_all();
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [network, url, state, reply, chunk]() {
        reply->deleteLater();

        const QByteArray data = reply->readAll();
        const int status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool failed     = reply->error() != QNetworkReply::NoError;
        int attempt{0};
        {
            const std::scoped_lock lock{state->mutex};

            if(failed && !isWholeBody(reply) && !state->closed && (attempt = ++state->attempts[chunk]) < MaxAttempts) {
                qCInfo(SUBSONIC_STREAM) << "Retrying chunk" << chunk << "after error:" << replyError(reply);
            }
            else {
                attempt = 0;
                state->pending.erase(chunk);
                state->attempts.erase(chunk);

                if(failed) {
                    state->error = replyError(reply);
                }
                else if(isWholeBody(reply)) {
                    state->body.append(data);
                    state->size         = state->body.size();
                    state->bodyComplete = true;
                }
                else if(status == PartialContentCode) {
                    if(const qint64 total = totalFromContentRange(reply->rawHeader("Content-Range")); total >= 0) {
                        state->size = total;
                    }
                    state->chunks[chunk] = data;
                }
            }
        }
        state->ready.notify_all();

        if(attempt > 0) {
            QTimer::singleShot(RetryDelay * attempt, network.get(),
                               [network, url, state, chunk]() { startChunkRequest(network, url, state, chunk); });
        }
    });
}

// Requires state->mutex
void fetchChunk(const std::shared_ptr<QNetworkAccessManager>& network, const QUrl& url,
                const std::shared_ptr<RangeStreamState>& state, qint64 chunk)
{
    if(state->wholeBody || state->chunks.contains(chunk) || state->pending.contains(chunk)
       || (state->size >= 0 && chunk * ChunkSize >= state->size)) {
        return;
    }

    state->pending.insert(chunk);
    QMetaObject::invokeMethod(network.get(),
                              [network, url, state, chunk]() { startChunkRequest(network, url, state, chunk); });
}
} // namespace

RangeStreamDevice::RangeStreamDevice(std::shared_ptr<QNetworkAccessManager> network, QUrl url, QObject* parent)
    : QIODevice{parent}
    , m_network{std::move(network)}
    , m_url{std::move(url)}
    , m_state{std::make_shared<RangeStreamState>()}
{ }

RangeStreamDevice::~RangeStreamDevice()
{
    RangeStreamDevice::close();
}

bool RangeStreamDevice::open(OpenMode mode)
{
    if(mode != ReadOnly) {
        return false;
    }

    if(QThread::currentThread() == m_network->thread()) {
        qCWarning(SUBSONIC_STREAM) << "Stream must not be opened from the network thread";
        return false;
    }

    std::unique_lock lock{m_state->mutex};
    m_state->closed = false;
    fetchChunk(m_network, m_url, m_state, 0);

    const bool answered = m_state->ready.wait_for(lock, RequestTimeout, [this]() {
        return m_state->size >= 0 || !m_state->body.isEmpty() || m_state->bodyComplete || !m_state->error.isEmpty();
    });
    if(!answered || !m_state->error.isEmpty()) {
        const QString error = answered ? m_state->error : tr("Timed out");
        qCWarning(SUBSONIC_STREAM) << "Failed to open stream:" << error;
        setErrorString(error);
        return false;
    }

    lock.unlock();
    return QIODevice::open(mode | Unbuffered);
}

void RangeStreamDevice::close()
{
    {
        const std::scoped_lock lock{m_state->mutex};
        m_state->closed = true;
        m_state->chunks.clear();
        m_state->body.clear();
    }
    m_state->ready.notify_all();
    if(isOpen()) {
        QIODevice::close();
    }
}

bool RangeStreamDevice::isSequential() const
{
    return false;
}

qint64 RangeStreamDevice::size() const
{
    const std::scoped_lock lock{m_state->mutex};
    return std::max<qint64>(m_state->size, m_state->body.size());
}

bool RangeStreamDevice::atEnd() const
{
    const std::scoped_lock lock{m_state->mutex};
    const bool sizeKnown = m_state->wholeBody ? m_state->bodyComplete : m_state->size >= 0;
    return sizeKnown && pos() >= std::max<qint64>(m_state->size, m_state->body.size());
}

qint64 RangeStreamDevice::readData(char* data, qint64 maxSize)
{
    const qint64 position = pos();
    const qint64 chunk    = position / ChunkSize;

    std::unique_lock lock{m_state->mutex};
    if(!m_state->wholeBody) {
        if(m_state->size >= 0 && position >= m_state->size) {
            return 0;
        }
        for(qint64 ahead{0}; ahead <= ChunksAhead; ++ahead) {
            fetchChunk(m_network, m_url, m_state, chunk + ahead);
        }
    }

    // Seeks past the downloaded part of a whole body wait for the download to get there
    const bool ready = m_state->ready.wait_for(lock, RequestTimeout, [this, chunk, position]() {
        if(m_state->closed || !m_state->error.isEmpty()) {
            return true;
        }
        if(m_state->wholeBody) {
            return m_state->bodyComplete || m_state->body.size() > position;
        }
        return m_state->chunks.contains(chunk);
    });
    if(!ready || m_state->closed || !m_state->error.isEmpty()) {
        setErrorString(ready ? m_state->error : tr("Timed out"));
        return -1;
    }

    if(m_state->wholeBody) {
        const qint64 count = std::min(maxSize, m_state->body.size() - position);
        if(count <= 0) {
            return 0;
        }
        std::memcpy(data, m_state->body.constData() + position, static_cast<size_t>(count));
        return count;
    }

    // Keep the chunk before the read position for decoders that step back, and the prefetched chunks after it
    std::erase_if(m_state->chunks,
                  [chunk](const auto& entry) { return entry.first < chunk - 1 || entry.first > chunk + ChunksAhead; });

    const QByteArray& chunkData = m_state->chunks.at(chunk);
    const qint64 offset         = position - (chunk * ChunkSize);
    const qint64 count          = std::min(maxSize, static_cast<qint64>(chunkData.size()) - offset);
    if(count <= 0) {
        return 0;
    }
    std::memcpy(data, chunkData.constData() + offset, static_cast<size_t>(count));
    return count;
}

qint64 RangeStreamDevice::writeData(const char* /*data*/, qint64 /*maxSize*/)
{
    return -1;
}
} // namespace Fooyin::Subsonic

#include "moc_rangestreamdevice.cpp"

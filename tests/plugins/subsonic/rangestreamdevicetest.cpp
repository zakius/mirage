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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
void ensureApplication()
{
    if(!QCoreApplication::instance()) {
        static int argc{1};
        static char name[]  = "rangestreamdevicetest";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);
    }
}

bool waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while(!condition() && timer.elapsed() < 5000) {
        QThread::msleep(10);
    }
    return condition();
}

//! A server that ignores range requests: it answers 200 with the whole body, sent as @p body.
class WholeBodyReply : public QNetworkReply
{
public:
    WholeBodyReply(const QNetworkRequest& request, QByteArray body, bool finishes, bool fails,
                   std::atomic_bool* aborted, QObject* parent)
        : QNetworkReply{parent}
        , m_body{std::move(body)}
        , m_aborted{aborted}
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        QIODevice::open(ReadOnly | Unbuffered);
        QTimer::singleShot(0, this, [this, finishes, fails]() {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
            setHeader(QNetworkRequest::ContentLengthHeader, 1000);
            Q_EMIT metaDataChanged();
            Q_EMIT readyRead();
            if(fails) {
                setError(RemoteHostClosedError, u"closed"_s);
            }
            if(finishes || fails) {
                setFinished(true);
                Q_EMIT finished();
            }
        });
    }

    void abort() override
    {
        if(!isFinished()) {
            m_aborted->store(true);
            setError(OperationCanceledError, u"aborted"_s);
            setFinished(true);
            Q_EMIT finished();
        }
    }

    [[nodiscard]] bool isSequential() const override
    {
        return true;
    }

    [[nodiscard]] qint64 bytesAvailable() const override
    {
        return m_body.size() + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 size = std::min<qint64>(m_body.size(), maxSize);
        std::memcpy(data, m_body.constData(), size);
        m_body.remove(0, size);
        return size;
    }

private:
    QByteArray m_body;
    std::atomic_bool* m_aborted;
};

class Server : public QNetworkAccessManager
{
public:
    //! Each request's reply: whether it finishes, and whether it fails after sending its data.
    std::vector<std::pair<bool, bool>> replies;
    std::atomic_int requests{0};
    std::atomic_bool aborted{false};

protected:
    QNetworkReply* createRequest(Operation /*operation*/, const QNetworkRequest& request,
                                 QIODevice* /*outgoingData*/) override
    {
        const auto index         = static_cast<size_t>(requests++);
        const auto [ends, fails] = index < replies.size() ? replies.at(index) : std::pair{false, false};
        return new WholeBodyReply(request, "0123456789"_ba, ends, fails, &aborted, this);
    }
};

class RangeStreamDeviceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApplication();
        m_server = std::shared_ptr<Server>{new Server, [](Server* server) { server->deleteLater(); }};
        m_server->moveToThread(&m_thread);
        m_thread.start();
    }

    void TearDown() override
    {
        m_thread.quit();
        m_thread.wait();
    }

    QThread m_thread;
    std::shared_ptr<Server> m_server;
};
} // namespace

TEST_F(RangeStreamDeviceTest, ClosingAbortsTheDownloadOfAServerWithoutRanges)
{
    {
        Subsonic::RangeStreamDevice device{m_server, QUrl{u"http://music.test/rest/stream?id=1"_s}};
        ASSERT_TRUE(device.open(QIODevice::ReadOnly));
        device.close();
        EXPECT_TRUE(waitFor([this]() { return m_server->aborted.load(); }));
    }
    QMetaObject::invokeMethod(m_server.get(), []() { }, Qt::BlockingQueuedConnection);
}

TEST_F(RangeStreamDeviceTest, OpensAgainAfterAFailedDownload)
{
    m_server->replies = {{false, true}, {true, false}};

    Subsonic::RangeStreamDevice device{m_server, QUrl{u"http://music.test/rest/stream?id=1"_s}};
    ASSERT_TRUE(device.open(QIODevice::ReadOnly));
    char data[20];
    EXPECT_TRUE(waitFor([&device, &data]() { return device.read(data, sizeof(data)) < 0 || device.atEnd(); }));
    device.close();

    ASSERT_TRUE(device.open(QIODevice::ReadOnly));
    EXPECT_EQ("0123456789"_ba, device.readAll());
    device.close();
    QMetaObject::invokeMethod(m_server.get(), []() { }, Qt::BlockingQueuedConnection);
}
} // namespace Fooyin::Testing

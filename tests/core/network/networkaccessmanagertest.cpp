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

#include <core/network/networkaccessmanager.h>
#include <core/network/networkutils.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
void ensureApplication()
{
    QStandardPaths::setTestModeEnabled(true);
    if(!QCoreApplication::instance()) {
        static int argc{1};
        static char name[]  = "networkaccessmanagertest";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);
    }
}

/*!
 * An HTTP server on 127.0.0.1. `/same` redirects to `/target` on 127.0.0.1, `/other` to `/target` on localhost, a
 * different host name for the same server; `/target` answers "ok". It records the Host of each request.
 */
class RedirectServer : public QTcpServer
{
public:
    RedirectServer()
    {
        EXPECT_TRUE(listen(QHostAddress::LocalHost));
        QObject::connect(this, &QTcpServer::newConnection, this, [this]() {
            while(QTcpSocket* socket = nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() { answer(socket); });
            }
        });
    }

    [[nodiscard]] QUrl url(const QString& path) const
    {
        return QUrl{u"http://127.0.0.1:%1%2?u=alice&t=token&s=salt"_s.arg(serverPort()).arg(path)};
    }

    QStringList requests;

private:
    void answer(QTcpSocket* socket)
    {
        const QByteArray request = socket->readAll();
        const QByteArray path    = request.split(' ').value(1).split('?').value(0);
        const QByteArray host    = request.mid(request.indexOf("Host: ") + 6).split('\r').value(0);
        requests.append(QString::fromLatin1(host + path));

        const QByteArray port = QByteArray::number(serverPort());
        QByteArray response;
        if(path == "/same") {
            response = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + port + "/target\r\n";
        }
        else if(path == "/other") {
            response = "HTTP/1.1 302 Found\r\nLocation: http://localhost:" + port + "/target\r\n";
        }
        else {
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
        }
        const QByteArray body = path == "/target" ? "ok" : "";
        socket->write(response + "Content-Length: " + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

class NetworkAccessManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApplication();
        m_settings = std::make_unique<SettingsManager>(m_dir.filePath(u"settings.ini"_s));
        m_network  = std::make_unique<NetworkAccessManager>(m_settings.get());
        m_server   = std::make_unique<RedirectServer>();
    }

    //! Fetches @p url and returns the body, or std::nullopt if the request failed.
    std::optional<QByteArray> fetch(const QUrl& url, NetworkRequestOptions options)
    {
        QNetworkReply* reply = m_network->get(makeNetworkRequest(url, options));
        QElapsedTimer timer;
        timer.start();
        while(!reply->isFinished() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        reply->deleteLater();
        if(!reply->isFinished() || reply->error() != QNetworkReply::NoError) {
            return {};
        }
        return reply->readAll();
    }

    QTemporaryDir m_dir;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<NetworkAccessManager> m_network;
    std::unique_ptr<RedirectServer> m_server;
};
} // namespace

TEST_F(NetworkAccessManagerTest, FollowsRedirectsWithCredentialsOnTheSameHost)
{
    EXPECT_EQ("ok"_ba, fetch(m_server->url(u"/same"_s), NetworkRequestOption::Credentials));
}

TEST_F(NetworkAccessManagerTest, RefusesRedirectsWithCredentialsToAnotherHost)
{
    EXPECT_FALSE(fetch(m_server->url(u"/other"_s), NetworkRequestOption::Credentials).has_value());
    EXPECT_EQ(1, m_server->requests.size());

    // Requests without credentials follow it
    EXPECT_EQ("ok"_ba, fetch(m_server->url(u"/other"_s), {}));
}

TEST_F(NetworkAccessManagerTest, KeepsRequestsWithCredentialsOutOfTheDiskCache)
{
    const QNetworkRequest request = makeNetworkRequest(m_server->url(u"/target"_s), NetworkRequestOption::Credentials);
    EXPECT_FALSE(request.attribute(QNetworkRequest::CacheSaveControlAttribute, true).toBool());
    EXPECT_EQ(QNetworkRequest::AlwaysNetwork, request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt());
}
} // namespace Fooyin::Testing

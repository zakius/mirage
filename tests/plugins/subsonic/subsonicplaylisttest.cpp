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

#include "subsonicplaylists.h"
#include "subsonicprovider.h"
#include "testutils.h"

#include "core/database/dbschema.h"
#include <core/coresettings.h>
#include <core/engine/audioloader.h>
#include <core/internalcoresettings.h>
#include <core/library/librarymanager.h>
#include <core/network/networkaccessmanager.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <numeric>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Testing {
namespace {
void ensureApplication()
{
    QStandardPaths::setTestModeEnabled(true);
    if(!QCoreApplication::instance()) {
        static int argc{1};
        static char name[]  = "subsonicplaylisttest";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);
    }
}

bool waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while(!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return condition();
}

void processEventsFor(std::chrono::milliseconds duration)
{
    QElapsedTimer timer;
    timer.start();
    while(timer.elapsed() < duration.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

//! A reply with a body that finishes after @p delay.
class Reply : public QNetworkReply
{
public:
    Reply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation, QByteArray body,
          std::chrono::milliseconds delay, QObject* parent)
        : QNetworkReply{parent}
        , m_body{std::move(body)}
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        QIODevice::open(ReadOnly | Unbuffered);
        QTimer::singleShot(delay, this, [this]() {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
            setFinished(true);
            Q_EMIT finished();
        });
    }

    void abort() override { }

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
};

//! A server with playlists, as the Subsonic API describes them.
class PlaylistServer : public NetworkAccessManager
{
public:
    struct ServerPlaylist
    {
        QString name;
        QString owner{u"alice"_s};
        QStringList songs;
    };

    struct Call
    {
        QString method;
        QUrlQuery params;
        bool post{false};
        qsizetype urlLength{0};
    };

    using NetworkAccessManager::NetworkAccessManager;

    bool formPost{false};
    //! getPlaylists answers this much later, as a slow server would.
    std::chrono::milliseconds getPlaylistsDelay{0};
    std::map<QString, ServerPlaylist> playlists;
    std::vector<Call> calls;

    [[nodiscard]] std::vector<Call> callsOf(const QString& method) const
    {
        std::vector<Call> result;
        std::ranges::copy_if(calls, std::back_inserter(result), [&method](const Call& call) {
            return call.method == method;
        });
        return result;
    }

    [[nodiscard]] bool changed() const
    {
        return std::ranges::any_of(calls, [](const Call& call) {
            return call.method == "createPlaylist"_L1 || call.method == "updatePlaylist"_L1
                || call.method == "deletePlaylist"_L1;
        });
    }

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override
    {
        const QString method = request.url().path().section(u'/', -1);
        const bool post      = operation == PostOperation;
        QUrlQuery params{request.url()};
        if(post) {
            params = QUrlQuery{QString::fromUtf8(outgoingData->readAll())};
        }
        calls.push_back({.method = method, .params = params, .post = post, .urlLength = request.url().toString().size()});

        const QJsonObject response = handle(method, params);
        const QByteArray data
            = QJsonDocument{QJsonObject{{u"subsonic-response"_s, response}}}.toJson(QJsonDocument::Compact);
        return new Reply(request, operation, data, method == "getPlaylists"_L1 ? getPlaylistsDelay : 0ms, this);
    }

private:
    static QJsonObject ok(QJsonObject object = {})
    {
        object.insert(u"status"_s, u"ok"_s);
        object.insert(u"version"_s, u"1.16.1"_s);
        return object;
    }

    static QJsonObject error(int code, const QString& message)
    {
        return {{u"status"_s, u"failed"_s},
                {u"version"_s, u"1.16.1"_s},
                {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, message}}}};
    }

    static QStringList values(const QUrlQuery& params, const QString& key)
    {
        return params.allQueryItemValues(key, QUrl::FullyDecoded);
    }

    QJsonObject playlistObject(const QString& id) const
    {
        const ServerPlaylist& playlist = playlists.at(id);
        QJsonArray entries;
        for(const QString& song : playlist.songs) {
            entries.append(QJsonObject{{u"id"_s, song}, {u"title"_s, QString{u"Song "_s + song}}});
        }
        return {{u"id"_s, id},
                {u"name"_s, playlist.name},
                {u"owner"_s, playlist.owner},
                {u"songCount"_s, playlist.songs.size()},
                {u"entry"_s, entries}};
    }

    QJsonObject handle(const QString& method, const QUrlQuery& params)
    {
        const QString id = params.queryItemValue(u"playlistId"_s, QUrl::FullyDecoded);

        if(method == "getUser"_L1) {
            return ok({{u"user"_s, QJsonObject{{u"username"_s, u"alice"_s}}}});
        }
        if(method == "getOpenSubsonicExtensions"_L1) {
            QJsonArray extensions;
            if(formPost) {
                extensions.append(QJsonObject{{u"name"_s, u"formPost"_s}, {u"versions"_s, QJsonArray{1}}});
            }
            return ok({{u"openSubsonicExtensions"_s, extensions}});
        }
        if(method == "getPlaylists"_L1) {
            QJsonArray list;
            for(const auto& [playlistId, playlist] : playlists) {
                list.append(QJsonObject{{u"id"_s, playlistId}, {u"name"_s, playlist.name}});
            }
            return ok({{u"playlists"_s, QJsonObject{{u"playlist"_s, list}}}});
        }
        if(method == "getPlaylist"_L1) {
            const QString playlistId = params.queryItemValue(u"id"_s);
            if(!playlists.contains(playlistId)) {
                return error(70, u"Playlist not found"_s);
            }
            return ok({{u"playlist"_s, playlistObject(playlistId)}});
        }
        if(method == "createPlaylist"_L1) {
            if(!id.isEmpty()) {
                if(!playlists.contains(id)) {
                    return error(70, u"Playlist not found"_s);
                }
                playlists.at(id).songs = values(params, u"songId"_s);
                return ok({{u"playlist"_s, playlistObject(id)}});
            }
            const QString newId = QString::number(m_nextId++);
            playlists[newId]    = {.name  = params.queryItemValue(u"name"_s, QUrl::FullyDecoded),
                                   .owner = u"alice"_s,
                                   .songs = values(params, u"songId"_s)};
            return ok({{u"playlist"_s, playlistObject(newId)}});
        }
        if(method == "updatePlaylist"_L1) {
            if(!playlists.contains(id)) {
                return error(70, u"Playlist not found"_s);
            }
            if(params.hasQueryItem(u"name"_s)) {
                playlists.at(id).name = params.queryItemValue(u"name"_s, QUrl::FullyDecoded);
            }
            playlists.at(id).songs.append(values(params, u"songIdToAdd"_s));
            return ok();
        }
        if(method == "deletePlaylist"_L1) {
            playlists.erase(params.queryItemValue(u"id"_s));
            return ok();
        }
        return error(0, u"Unknown method"_s);
    }

    int m_nextId{100};
};

Track libraryTrack(const QString& libraryPath, int number)
{
    Track track{libraryPath + u"/song/"_s + QString::number(number)};
    track.setId(number);
    track.setTitle(u"Song %1"_s.arg(number));
    track.generateHash();
    return track;
}

QStringList songNumbers(int first, int last)
{
    QStringList songs;
    for(int number{first}; number <= last; ++number) {
        songs.append(QString::number(number));
    }
    return songs;
}

class SubsonicPlaylistTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApplication();
        ASSERT_TRUE(m_dir.isValid());

        m_settings = std::make_unique<SettingsManager>(m_dir.filePath(u"settings.ini"_s));
        m_settings->createSetting<Settings::Core::ShuffleAlbumsGroupScript>(u"%album%"_s,
                                                                            u"Playback/ShuffleAlbumsGroupScript"_s);
        m_settings->createSetting<Settings::Core::ShuffleAlbumsSortScript>(u"%track%"_s,
                                                                           u"Playback/ShuffleAlbumsSortScript"_s);

        DbConnection::DbParams params;
        params.type     = u"QSQLITE"_s;
        params.filePath = m_dir.filePath(u"mirage.db"_s);
        m_dbPool        = DbConnectionPool::create(params, u"subsonic-playlist-test"_s);
        m_dbHandler     = std::make_unique<DbConnectionHandler>(m_dbPool);
        DbSchema schema{DbConnectionProvider{m_dbPool}};
        ASSERT_NE(DbSchema::UpgradeResult::Failed, schema.upgradeDatabase(22, u"://dbschema.xml"_s));

        m_server = std::make_shared<PlaylistServer>(m_settings.get());
        m_server->playlists[u"1"_s] = {.name = u"Mix"_s, .owner = u"alice"_s, .songs = {u"1"_s, u"2"_s}};

        m_libraryManager = std::make_unique<LibraryManager>(m_dbPool, m_settings.get());
        m_library        = std::make_unique<StubMusicLibrary>();
        m_handler = std::make_unique<PlaylistHandler>(m_dbPool, std::make_shared<AudioLoader>(), m_library.get(),
                                                      m_settings.get());
        m_provider = std::make_unique<Subsonic::SubsonicProvider>(m_server, m_dbPool);
        m_mirror   = std::make_unique<Subsonic::PlaylistMirror>(m_provider.get(), m_libraryManager.get(),
                                                                m_library.get(), m_handler.get());
        m_mirror->setPushDelay(10ms);
    }

    void TearDown() override
    {
        m_mirror.reset();
        m_handler.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    //! Adds the server as a library and applies its playlists.
    void addServer(const std::vector<int>& songs = {1, 2, 3})
    {
        m_libraryPath = Subsonic::SubsonicProvider::libraryPath(m_account);
        ASSERT_TRUE(m_provider->saveAccount(m_libraryPath, m_account));
        bool answered{false};
        m_provider->fetchServerInfo(m_libraryPath, m_account, [&answered]() { answered = true; });
        ASSERT_TRUE(waitFor([&answered]() { return answered; }));
        m_libraryId = m_libraryManager->addLibrary(m_libraryPath, u"Server"_s);
        ASSERT_GT(m_libraryId, 0);

        TrackList tracks;
        for(const int number : songs) {
            tracks.push_back(libraryTrack(m_libraryPath, number));
        }
        m_library->setLibraryTracks(tracks);
        sync();
    }

    void sync()
    {
        const auto before = m_server->callsOf(u"getPlaylist"_s).size();
        m_mirror->syncPlaylists(m_libraryPath);
        ASSERT_TRUE(waitFor([this, before]() {
            return m_server->callsOf(u"getPlaylist"_s).size() >= before + m_server->playlists.size();
        }));
        processEventsFor(50ms);
    }

    Playlist* playlistNamed(const QString& name) const
    {
        return m_handler->playlistByName(name);
    }

    QTemporaryDir m_dir;
    std::unique_ptr<SettingsManager> m_settings;
    DbConnectionPoolPtr m_dbPool;
    std::unique_ptr<DbConnectionHandler> m_dbHandler;
    std::shared_ptr<PlaylistServer> m_server;
    std::unique_ptr<LibraryManager> m_libraryManager;
    std::unique_ptr<StubMusicLibrary> m_library;
    std::unique_ptr<PlaylistHandler> m_handler;
    std::unique_ptr<Subsonic::SubsonicProvider> m_provider;
    std::unique_ptr<Subsonic::PlaylistMirror> m_mirror;
    Subsonic::Account m_account{
        .url = QUrl{u"http://music.test:4533"_s}, .username = u"alice"_s, .password = u"pw"_s, .scrobble = {}};
    QString m_libraryPath;
    int m_libraryId{-1};
};
} // namespace

TEST_F(SubsonicPlaylistTest, MirrorsTheServersPlaylists)
{
    addServer();
    auto* playlist = playlistNamed(u"Mix"_s);
    ASSERT_NE(nullptr, playlist);
    EXPECT_EQ(2, playlist->trackCount());
    EXPECT_FALSE(playlist->isLocked());
    EXPECT_FALSE(m_server->changed());
}

TEST_F(SubsonicPlaylistTest, RemovingTheServerLeavesItsPlaylistsAlone)
{
    addServer();
    auto* playlist = playlistNamed(u"Mix"_s);
    ASSERT_NE(nullptr, playlist);

    // As the plugin does, the account goes first; the library then deletes the server's tracks
    QObject::connect(m_libraryManager.get(), &LibraryManager::libraryAboutToBeRemoved, m_provider.get(),
                     [this](const LibraryInfo& library) { m_provider->removeAccount(library.path); });
    const TrackList tracks = m_library->libraryTracks();
    ASSERT_TRUE(m_libraryManager->removeLibrary(m_libraryId));
    m_handler->handleTracksDeleted(tracks);
    EXPECT_EQ(0, playlist->trackCount());

    processEventsFor(100ms);
    EXPECT_FALSE(m_server->changed());
    EXPECT_EQ((QStringList{u"1"_s, u"2"_s}), m_server->playlists.at(u"1"_s).songs);
}

TEST_F(SubsonicPlaylistTest, RemovingUnavailableTracksIsNotAnEdit)
{
    addServer();
    m_handler->handleTracksDeleted({libraryTrack(m_libraryPath, 2)});
    EXPECT_EQ(1, playlistNamed(u"Mix"_s)->trackCount());

    processEventsFor(100ms);
    EXPECT_FALSE(m_server->changed());
}

TEST_F(SubsonicPlaylistTest, EditsReachTheServer)
{
    addServer();
    auto* playlist = playlistNamed(u"Mix"_s);
    m_handler->appendToPlaylist(playlist->id(), {libraryTrack(m_libraryPath, 3)});

    ASSERT_TRUE(waitFor([this]() { return m_server->playlists.at(u"1"_s).songs.size() == 3; }));
    EXPECT_EQ((QStringList{u"1"_s, u"2"_s, u"3"_s}), m_server->playlists.at(u"1"_s).songs);
}

TEST_F(SubsonicPlaylistTest, UndoingARemovalCreatesThePlaylistAgain)
{
    addServer();
    auto* playlist = playlistNamed(u"Mix"_s);
    const UId id   = playlist->id();

    m_handler->removePlaylist(id);
    ASSERT_TRUE(waitFor([this]() { return m_server->playlists.empty(); }));

    ASSERT_NE(nullptr, m_handler->restorePlaylist(id));
    ASSERT_TRUE(waitFor([this]() { return m_server->playlists.size() == 1; }));
    const auto& [serverId, restored] = *m_server->playlists.begin();
    EXPECT_EQ(u"Mix"_s, restored.name);
    EXPECT_EQ((QStringList{u"1"_s, u"2"_s}), restored.songs);

    // It stays mirrored
    sync();
    ASSERT_NE(nullptr, m_handler->playlistById(id));
    EXPECT_EQ(m_libraryPath + u"/playlist/"_s + serverId,
              m_handler->playlistById(id)->extraProperties().value(u"SubsonicPlaylist"_s));
    EXPECT_EQ(1, m_server->playlists.size());
}

TEST_F(SubsonicPlaylistTest, SendsLargePlaylistsInBatchesWithoutFormPost)
{
    std::vector<int> songs(251);
    std::iota(songs.begin(), songs.end(), 1);
    m_server->playlists[u"1"_s].songs = songNumbers(1, 250);
    addServer(songs);
    auto* playlist = playlistNamed(u"Mix"_s);
    ASSERT_EQ(250, playlist->trackCount());

    m_handler->appendToPlaylist(playlist->id(), {libraryTrack(m_libraryPath, 251)});
    ASSERT_TRUE(waitFor([this]() { return m_server->callsOf(u"updatePlaylist"_s).size() == 3; }));
    processEventsFor(50ms);

    EXPECT_EQ(songNumbers(1, 251), m_server->playlists.at(u"1"_s).songs);
    for(const auto& call : m_server->calls) {
        EXPECT_FALSE(call.post);
        EXPECT_LT(call.urlLength, 4000);
    }
}

TEST_F(SubsonicPlaylistTest, SendsLargePlaylistsAsOneFormWithFormPost)
{
    m_server->formPost = true;
    std::vector<int> songs(251);
    std::iota(songs.begin(), songs.end(), 1);
    m_server->playlists[u"1"_s].songs = songNumbers(1, 250);
    addServer(songs);
    auto* playlist = playlistNamed(u"Mix"_s);

    m_handler->appendToPlaylist(playlist->id(), {libraryTrack(m_libraryPath, 251)});
    ASSERT_TRUE(waitFor([this]() { return m_server->callsOf(u"updatePlaylist"_s).size() == 1; }));

    EXPECT_EQ(songNumbers(1, 251), m_server->playlists.at(u"1"_s).songs);
    const auto creates = m_server->callsOf(u"createPlaylist"_s);
    ASSERT_EQ(1, creates.size());
    EXPECT_TRUE(creates.front().post);
    // The credentials are in the form too, not in the URL
    EXPECT_TRUE(creates.front().params.hasQueryItem(u"t"_s));
    EXPECT_LT(creates.front().urlLength, 100);
}

TEST_F(SubsonicPlaylistTest, ASyncInFlightKeepsAPlaylistCreatedMeanwhile)
{
    addServer();
    m_server->getPlaylistsDelay = 300ms;
    m_mirror->syncPlaylists(m_libraryPath);

    auto* created = m_handler->createPlaylist(u"New"_s, {libraryTrack(m_libraryPath, 3)});
    ASSERT_NE(nullptr, created);
    ASSERT_TRUE(waitFor([created]() { return created->hasExtraProperty(u"SubsonicPlaylist"_s); }));

    processEventsFor(500ms);
    EXPECT_NE(nullptr, playlistNamed(u"New"_s));
    EXPECT_EQ(2, m_server->playlists.size());
}

TEST_F(SubsonicPlaylistTest, ComparesOwnersWithoutCase)
{
    m_server->playlists[u"1"_s].owner = u"Alice"_s;
    m_server->playlists[u"2"_s]       = {.name = u"Bob's"_s, .owner = u"bob"_s, .songs = {u"3"_s}};
    addServer();

    ASSERT_NE(nullptr, playlistNamed(u"Mix"_s));
    EXPECT_FALSE(playlistNamed(u"Mix"_s)->isLocked());
    ASSERT_NE(nullptr, playlistNamed(u"Bob's"_s));
    EXPECT_TRUE(playlistNamed(u"Bob's"_s)->isLocked());
}
} // namespace Fooyin::Testing

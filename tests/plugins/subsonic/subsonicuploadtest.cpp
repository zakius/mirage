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

#include "subsonicprovider.h"
#include "subsonicupload.h"

#include "../cdda/cddadiscripper.h"
#include "../cdda/cddatoc.h"

#include "core/database/dbschema.h"
#include <core/network/networkaccessmanager.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include <gtest/gtest.h>

#include <cstring>
#include <map>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
void ensureApplication()
{
    if(!QCoreApplication::instance()) {
        static int argc{1};
        static char name[] = "subsonicuploadtest";
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

//! A reply that finishes on the next event loop turn, with a body or a network error.
class Reply : public QNetworkReply
{
public:
    Reply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation, QByteArray body,
          bool networkError, QObject* parent)
        : QNetworkReply{parent}
        , m_body{std::move(body)}
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        QIODevice::open(ReadOnly | Unbuffered);
        QTimer::singleShot(0, this, [this, networkError]() {
            if(m_finished) {
                return;
            }
            if(networkError) {
                setError(RemoteHostClosedError, u"closed"_s);
                m_body.clear();
            }
            else {
                setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
            }
            finish();
        });
    }

    void abort() override
    {
        if(!m_finished) {
            setError(OperationCanceledError, u"aborted"_s);
            m_body.clear();
            finish();
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
    void finish()
    {
        m_finished = true;
        setFinished(true);
        Q_EMIT finished();
    }

    QByteArray m_body;
    bool m_finished{false};
};

/*!
 * A server with the `upload` extension, as the extension describes it. It keeps uploads in memory and can drop the
 * connection during an uploadTrack after storing part of the chunk.
 */
class FakeServer : public NetworkAccessManager
{
public:
    struct Slot
    {
        int disc{1};
        int number{0};
        qint64 size{-1};
        QByteArray data;
    };

    using NetworkAccessManager::NetworkAccessManager;

    bool uploadExtension{true};
    bool uploadRole{true};
    bool albumExists{false};
    //! The uploadTrack call (counting from 1) that stores two bytes and then drops the connection.
    int dropUploadTrack{0};
    //! The uploadTrack call (counting from 1) that the server refuses with an API error.
    int refuseUploadTrack{0};
    //! Slots for a createUpload with a releaseId.
    std::vector<std::pair<int, int>> releaseSlots{{1, 1}, {1, 2}};

    QStringList calls;
    std::vector<QUrlQuery> queries;
    std::map<QString, std::vector<Slot>> uploads;
    QStringList cancelled;
    QJsonObject createdTags;
    bool committed{false};

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override
    {
        const QByteArray body = outgoingData ? outgoingData->readAll() : QByteArray{};
        const QString method  = request.url().path().section(u'/', -1);
        const QUrlQuery query{request.url()};
        calls.append(method);
        queries.push_back(query);

        bool drop{false};
        const QJsonObject response = handle(method, query, body, drop);
        const QByteArray data
            = QJsonDocument{QJsonObject{{u"subsonic-response"_s, response}}}.toJson(QJsonDocument::Compact);
        return new Reply(request, operation, data, drop, this);
    }

private:
    static QJsonObject ok(QJsonObject object = {})
    {
        object.insert(u"status"_s, u"ok"_s);
        object.insert(u"version"_s, u"1.16.1"_s);
        object.insert(u"openSubsonic"_s, true);
        return object;
    }

    static QJsonObject error(int code, const QString& message)
    {
        return {{u"status"_s, u"failed"_s},
                {u"version"_s, u"1.16.1"_s},
                {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, message}}}};
    }

    static QJsonObject uploadObject(const QString& id, const std::vector<Slot>& slots, bool withSizes)
    {
        QJsonArray tracks;
        for(const Slot& slot : slots) {
            QJsonObject track{{u"disc"_s, slot.disc}, {u"number"_s, slot.number}};
            if(withSizes) {
                track.insert(u"received"_s, slot.data.size());
                track.insert(u"size"_s, slot.size);
            }
            tracks.append(track);
        }
        return {{u"id"_s, id}, {u"expires"_s, u"2026-11-02T22:00:00Z"_s}, {u"track"_s, tracks}};
    }

    QJsonObject handle(const QString& method, const QUrlQuery& query, const QByteArray& body, bool& drop)
    {
        const QString uploadId = query.queryItemValue(u"uploadId"_s);

        if(method == "getOpenSubsonicExtensions"_L1) {
            QJsonArray extensions{QJsonObject{{u"name"_s, u"songLyrics"_s}, {u"versions"_s, QJsonArray{1}}}};
            if(uploadExtension) {
                extensions.append(QJsonObject{{u"name"_s, u"upload"_s}, {u"versions"_s, QJsonArray{1}}});
            }
            return ok({{u"openSubsonicExtensions"_s, extensions}});
        }
        if(method == "getUser"_L1) {
            return ok({{u"user"_s, QJsonObject{{u"username"_s, u"alice"_s}, {u"uploadRole"_s, uploadRole}}}});
        }
        if(method == "createUpload"_L1) {
            std::vector<Slot> slots;
            if(query.hasQueryItem(u"releaseId"_s)) {
                for(const auto& [disc, number] : releaseSlots) {
                    slots.push_back({.disc = disc, .number = number, .size = -1, .data = {}});
                }
            }
            else {
                createdTags = QJsonDocument::fromJson(
                                  query.queryItemValue(u"tags"_s, QUrl::FullyDecoded).toUtf8())
                                  .object();
                for(const auto& track : createdTags.value("track"_L1).toArray()) {
                    slots.push_back({.disc   = track.toObject().value("disc"_L1).toInt(),
                                     .number = track.toObject().value("number"_L1).toInt(),
                                     .size   = -1,
                                     .data   = {}});
                }
            }
            const QString id = u"up-%1"_s.arg(uploads.size() + 1);
            uploads[id]      = slots;
            return ok({{u"upload"_s, uploadObject(id, slots, false)}});
        }
        if(method == "uploadTrack"_L1) {
            if(!uploads.contains(uploadId)) {
                return error(70, u"Upload not found"_s);
            }
            auto& slots     = uploads.at(uploadId);
            const int disc  = query.queryItemValue(u"disc"_s).toInt();
            const int track = query.queryItemValue(u"number"_s).toInt();
            const auto slot = std::ranges::find_if(
                slots, [disc, track](const Slot& s) { return s.disc == disc && s.number == track; });
            if(slot == slots.end()) {
                return error(70, u"No such track"_s);
            }
            if(query.queryItemValue(u"offset"_s).toLongLong() != slot->data.size()) {
                return error(0, u"Wrong offset"_s);
            }
            slot->size = query.queryItemValue(u"size"_s).toLongLong();
            const int call = static_cast<int>(calls.count(u"uploadTrack"_s));
            if(call == refuseUploadTrack) {
                return error(0, u"No space left on the server"_s);
            }
            if(call == dropUploadTrack) {
                slot->data.append(body.left(2));
                drop = true;
                return {};
            }
            slot->data.append(body);
            return ok({{u"uploadTrack"_s, QJsonObject{{u"received"_s, slot->data.size()}}}});
        }
        if(method == "getUpload"_L1) {
            const QString id = query.queryItemValue(u"id"_s);
            if(!uploads.contains(id)) {
                return error(70, u"Upload not found"_s);
            }
            return ok({{u"upload"_s, uploadObject(id, uploads.at(id), true)}});
        }
        if(method == "commitUpload"_L1) {
            if(!uploads.contains(uploadId)) {
                return error(70, u"Upload not found"_s);
            }
            for(const Slot& slot : uploads.at(uploadId)) {
                if(slot.data.size() != slot.size) {
                    return error(0, u"Track %1 is incomplete"_s.arg(slot.number));
                }
            }
            if(albumExists && query.queryItemValue(u"replace"_s) != "true"_L1) {
                return error(0, u"Album already in the library"_s);
            }
            committed = true;
            return ok({{u"album"_s, QJsonObject{{u"id"_s, u"al-1"_s},
                                                 {u"name"_s, createdTags.value("album"_L1).toString()},
                                                 {u"song"_s, QJsonArray{}}}}});
        }
        if(method == "cancelUpload"_L1) {
            uploads.erase(uploadId);
            cancelled.append(uploadId);
            return ok();
        }
        if(method == "lookupDisc"_L1) {
            const QJsonObject release{
                {u"id"_s, u"f1c2-release"_s},
                {u"title"_s, u"Album"_s},
                {u"artist"_s, u"Artist"_s},
                {u"year"_s, 1999},
                {u"country"_s, u"JP"_s},
                {u"discCount"_s, 2},
                {u"discNumber"_s, 2},
                {u"track"_s, QJsonArray{QJsonObject{{u"number"_s, 1}, {u"title"_s, u"One"_s}, {u"artist"_s, u"A"_s}},
                                        QJsonObject{{u"number"_s, 2}, {u"title"_s, u"Two"_s}, {u"artist"_s, u"B"_s}}}}};
            return ok({{u"discLookup"_s, QJsonObject{{u"release"_s, QJsonArray{release}}}}});
        }
        return error(0, u"Unknown method"_s);
    }
};

class SubsonicUploadTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApplication();
        ASSERT_TRUE(m_dir.isValid());
        m_settings = std::make_unique<SettingsManager>(m_dir.filePath(u"settings.ini"_s));
        m_server   = std::make_shared<FakeServer>(m_settings.get());
        m_account  = {.url = QUrl{u"http://music.test:4533"_s}, .username = u"alice"_s, .password = u"pw"_s, .scrobble = {}};
    }

    QString writeFile(const QString& name, const QByteArray& data)
    {
        const QString path = m_dir.filePath(name);
        QFile file{path};
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(data);
        return path;
    }

    Subsonic::UploadTags albumOf(const QStringList& files)
    {
        Subsonic::UploadTags tags;
        tags.album       = u"Album"_s;
        tags.albumArtist = u"Artist"_s;
        tags.year        = u"1999"_s;
        int number{0};
        for(const QString& file : files) {
            ++number;
            tags.tracks.push_back(
                {.disc = 1, .number = number, .title = u"Track %1"_s.arg(number), .artist = u"Artist"_s, .file = file});
        }
        return tags;
    }

    QTemporaryDir m_dir;
    std::unique_ptr<SettingsManager> m_settings;
    std::shared_ptr<FakeServer> m_server;
    Subsonic::Account m_account;
};
} // namespace

TEST_F(SubsonicUploadTest, UploadsEveryFileInChunksAndCommits)
{
    const QByteArray first{"0123456789"};
    const QByteArray second{"abcdefg"};
    const QString workFolder = m_dir.filePath(u"rip"_s);
    ASSERT_TRUE(QDir{}.mkpath(workFolder));

    Subsonic::Uploader uploader{m_server.get(), m_account,
                                albumOf({writeFile(u"1.flac"_s, first), writeFile(u"2.flac"_s, second)})};
    uploader.setChunkSize(4);
    uploader.setWorkFolder(workFolder);

    std::optional<QJsonObject> album;
    QObject::connect(&uploader, &Subsonic::Uploader::committed, [&album](const QJsonObject& a) { album = a; });
    uploader.start();

    ASSERT_TRUE(waitFor([&album]() { return album.has_value(); }));
    EXPECT_EQ(u"Album"_s, album->value("name"_L1).toString());
    EXPECT_EQ(first, m_server->uploads.at(u"up-1"_s).at(0).data);
    EXPECT_EQ(second, m_server->uploads.at(u"up-1"_s).at(1).data);
    // 10 bytes in three chunks and 7 in two
    EXPECT_EQ(5, m_server->calls.count(u"uploadTrack"_s));
    EXPECT_EQ(u"commitUpload"_s, m_server->calls.constLast());
    EXPECT_EQ(u"flac"_s, m_server->queries.at(1).queryItemValue(u"suffix"_s));
    EXPECT_EQ(u"10"_s, m_server->queries.at(1).queryItemValue(u"size"_s));

    EXPECT_EQ(u"Album"_s, m_server->createdTags.value("album"_L1).toString());
    EXPECT_EQ(1999, m_server->createdTags.value("year"_L1).toInt());
    const QJsonObject track = m_server->createdTags.value("track"_L1).toArray().at(1).toObject();
    EXPECT_EQ(2, track.value("number"_L1).toInt());
    EXPECT_EQ(u"Track 2"_s, track.value("title"_L1).toString());

    EXPECT_FALSE(QFileInfo::exists(workFolder));
}

TEST_F(SubsonicUploadTest, ResumesFromWhatTheServerStoredAfterAnInterruptedChunk)
{
    const QByteArray data{"0123456789ABCDEF"};
    Subsonic::Uploader uploader{m_server.get(), m_account, albumOf({writeFile(u"1.flac"_s, data)})};
    uploader.setChunkSize(8);
    // The second chunk stores two of its bytes before the connection drops
    m_server->dropUploadTrack = 2;

    bool committed{false};
    QObject::connect(&uploader, &Subsonic::Uploader::committed, [&committed]() { committed = true; });
    uploader.start();

    ASSERT_TRUE(waitFor([&committed]() { return committed; }));
    EXPECT_EQ(data, m_server->uploads.at(u"up-1"_s).front().data);
    EXPECT_EQ(1, m_server->calls.count(u"getUpload"_s));

    const auto getUpload = m_server->calls.indexOf(u"getUpload"_s);
    EXPECT_EQ(u"10"_s, m_server->queries.at(getUpload + 1).queryItemValue(u"offset"_s));
}

TEST_F(SubsonicUploadTest, StopsOnAServerErrorAndContinuesWhenRetried)
{
    const QByteArray data{"0123456789"};
    Subsonic::Uploader uploader{m_server.get(), m_account, albumOf({writeFile(u"1.flac"_s, data)})};
    uploader.setChunkSize(4);
    m_server->refuseUploadTrack = 2;

    std::optional<Subsonic::ApiError> failure;
    bool committed{false};
    QObject::connect(&uploader, &Subsonic::Uploader::failed,
                     [&failure](const Subsonic::ApiError& error) { failure = error; });
    QObject::connect(&uploader, &Subsonic::Uploader::committed, [&committed]() { committed = true; });
    uploader.start();

    ASSERT_TRUE(waitFor([&failure]() { return failure.has_value(); }));
    EXPECT_EQ(u"No space left on the server"_s, failure->message);
    EXPECT_FALSE(Subsonic::isDuplicateError(*failure));
    EXPECT_FALSE(m_server->calls.contains(u"getUpload"_s));

    uploader.start();
    ASSERT_TRUE(waitFor([&committed]() { return committed; }));
    EXPECT_EQ(data, m_server->uploads.at(u"up-1"_s).front().data);
    EXPECT_EQ(1, m_server->calls.count(u"createUpload"_s));
}

TEST_F(SubsonicUploadTest, OffersReplaceOnlyForAnAlbumAlreadyInTheLibrary)
{
    m_server->albumExists = true;
    Subsonic::Uploader uploader{m_server.get(), m_account, albumOf({writeFile(u"1.flac"_s, "data")})};

    std::optional<Subsonic::ApiError> failure;
    bool committed{false};
    QObject::connect(&uploader, &Subsonic::Uploader::failed,
                     [&failure](const Subsonic::ApiError& error) { failure = error; });
    QObject::connect(&uploader, &Subsonic::Uploader::committed, [&committed]() { committed = true; });
    uploader.start();

    ASSERT_TRUE(waitFor([&failure]() { return failure.has_value(); }));
    EXPECT_TRUE(Subsonic::isDuplicateError(*failure));
    EXPECT_FALSE(m_server->committed);

    uploader.replace();
    ASSERT_TRUE(waitFor([&committed]() { return committed; }));
    EXPECT_EQ(u"true"_s, m_server->queries.back().queryItemValue(u"replace"_s));

    EXPECT_FALSE(Subsonic::isDuplicateError({.code = 0, .message = u"Track 1 is incomplete"_s}));
    EXPECT_FALSE(Subsonic::isDuplicateError({.code = 50, .message = u"already in the library"_s}));
    EXPECT_FALSE(Subsonic::isDuplicateError({.code = -1, .message = u"HTTP 500"_s}));
}

TEST_F(SubsonicUploadTest, CancelDeletesTheUploadAndTheWorkFolder)
{
    const QString workFolder = m_dir.filePath(u"rip"_s);
    ASSERT_TRUE(QDir{}.mkpath(workFolder));
    writeFile(u"rip/1.flac"_s, "0123456789");

    Subsonic::Uploader uploader{m_server.get(), m_account, albumOf({m_dir.filePath(u"rip/1.flac"_s)})};
    uploader.setChunkSize(2);
    uploader.setWorkFolder(workFolder);

    bool cancelled{false};
    QObject::connect(&uploader, &Subsonic::Uploader::cancelled, [&cancelled]() { cancelled = true; });
    QObject::connect(&uploader, &Subsonic::Uploader::progress, &uploader, [&uploader](qint64 sent, qint64) {
        if(sent == 4) {
            uploader.cancel();
        }
    });
    uploader.start();

    ASSERT_TRUE(waitFor([&cancelled]() { return cancelled; }));
    EXPECT_EQ(QStringList{u"up-1"_s}, m_server->cancelled);
    EXPECT_FALSE(m_server->calls.contains(u"commitUpload"_s));
    EXPECT_FALSE(QFileInfo::exists(workFolder));
}

TEST_F(SubsonicUploadTest, CancelBeforeTheUploadExistsOnlyDeletesTheWorkFolder)
{
    const QString workFolder = m_dir.filePath(u"rip"_s);
    ASSERT_TRUE(QDir{}.mkpath(workFolder));

    Subsonic::Uploader uploader{m_server.get(), m_account, albumOf({writeFile(u"1.flac"_s, "data")})};
    uploader.setWorkFolder(workFolder);
    bool cancelled{false};
    QObject::connect(&uploader, &Subsonic::Uploader::cancelled, [&cancelled]() { cancelled = true; });
    uploader.cancel();

    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(m_server->calls.contains(u"cancelUpload"_s));
    EXPECT_FALSE(QFileInfo::exists(workFolder));
}

TEST_F(SubsonicUploadTest, UploadsAReleaseByItsMusicBrainzId)
{
    Subsonic::UploadTags tags = albumOf({writeFile(u"1.flac"_s, "one"), writeFile(u"2.flac"_s, "two")});
    tags.releaseId            = u"f1c2-release"_s;
    tags.releaseDisc          = 2;
    for(auto& track : tags.tracks) {
        track.disc = 2;
    }
    m_server->releaseSlots = {{2, 1}, {2, 2}};

    Subsonic::Uploader uploader{m_server.get(), m_account, tags};
    bool committed{false};
    QObject::connect(&uploader, &Subsonic::Uploader::committed, [&committed]() { committed = true; });
    uploader.start();

    ASSERT_TRUE(waitFor([&committed]() { return committed; }));
    const QUrlQuery& create = m_server->queries.front();
    EXPECT_EQ(u"f1c2-release"_s, create.queryItemValue(u"releaseId"_s));
    EXPECT_EQ(u"2"_s, create.queryItemValue(u"disc"_s));
    EXPECT_FALSE(create.hasQueryItem(u"tags"_s));
}

TEST_F(SubsonicUploadTest, StopsWhenTheServerExpectsATrackItWasNotGiven)
{
    Subsonic::UploadTags tags = albumOf({writeFile(u"1.flac"_s, "one")});
    tags.releaseId            = u"f1c2-release"_s;

    Subsonic::Uploader uploader{m_server.get(), m_account, tags};
    std::optional<Subsonic::ApiError> failure;
    QObject::connect(&uploader, &Subsonic::Uploader::failed,
                     [&failure](const Subsonic::ApiError& error) { failure = error; });
    uploader.start();

    ASSERT_TRUE(waitFor([&failure]() { return failure.has_value(); }));
    EXPECT_FALSE(m_server->calls.contains(u"uploadTrack"_s));
}

TEST_F(SubsonicUploadTest, OffersUploadsOnlyWithTheExtensionAndTheUploadRole)
{
    const QString dbPath = m_dir.filePath(u"mirage.db"_s);
    DbConnection::DbParams params;
    params.type     = u"QSQLITE"_s;
    params.filePath = dbPath;
    auto dbPool     = DbConnectionPool::create(params, u"subsonic-upload-test"_s);
    const DbConnectionHandler handler{dbPool};
    DbSchema schema{DbConnectionProvider{dbPool}};
    std::ignore = schema.upgradeDatabase(22, u"://dbschema.xml"_s);

    Subsonic::SubsonicProvider provider{m_server, dbPool};
    const QString libraryPath = Subsonic::SubsonicProvider::libraryPath(m_account);
    ASSERT_TRUE(provider.saveAccount(libraryPath, m_account));

    const auto canUpload = [&](bool extension, bool role) {
        m_server->uploadExtension = extension;
        m_server->uploadRole      = role;
        bool done{false};
        provider.fetchServerInfo(libraryPath, m_account, [&done]() { done = true; });
        EXPECT_TRUE(waitFor([&done]() { return done; }));
        return provider.canUpload(libraryPath);
    };

    EXPECT_TRUE(canUpload(true, true));
    EXPECT_FALSE(canUpload(false, true));
    EXPECT_FALSE(canUpload(true, false));
    EXPECT_FALSE(canUpload(false, false));
    EXPECT_TRUE(canUpload(true, true));
    EXPECT_TRUE(provider.canUpload(libraryPath + u"/song/1"_s));
    EXPECT_FALSE(provider.canUpload(u"subsonic://bob@other:4533"_s));
}

TEST_F(SubsonicUploadTest, AsksLookupDiscWithTheDiscIdAndTocOfTheDisc)
{
    // A published disc: MusicBrainz offsets, with the lead-in, of twelve tracks
    const std::vector<int> offsets{150, 22767, 41887, 58317, 72102, 91375, 104652, 115380, 132165, 143932, 159870, 174597};
    Cdda::CdToc toc{.firstTrackNumber = 1, .lastTrackNumber = 12, .leadoutSector = 267257 - 150, .tracks = {}};
    for(size_t i{0}; i < offsets.size(); ++i) {
        const int end = i + 1 < offsets.size() ? offsets.at(i + 1) : 267257;
        toc.tracks.push_back({.number             = static_cast<int>(i + 1),
                              .firstSector        = offsets.at(i) - 150,
                              .endSectorExclusive = end - 150,
                              .isAudio            = true});
    }

    // The audio CD plugin sets these on the disc's tracks, and the upload dialog reads them back
    Track track{u"cdda://disc"_s, 0};
    track.setExtraProperty(QString::fromLatin1(Cdda::DiscIdProperty), *Cdda::musicBrainzDiscId(toc));
    track.setExtraProperty(QString::fromLatin1(Cdda::DiscTocProperty), *Cdda::musicBrainzToc(toc));

    std::vector<Subsonic::DiscRelease> releases;
    Subsonic::request(m_server.get(), m_account, u"lookupDisc"_s,
                      Subsonic::lookupDiscQuery(Cdda::discProperty(track, Cdda::DiscIdProperty),
                                                Cdda::discProperty(track, Cdda::DiscTocProperty)),
                      m_server.get(), [&releases](std::optional<QJsonObject> response, const Subsonic::ApiError&) {
                          releases = Subsonic::discReleases(response.value_or(QJsonObject{}));
                      });
    ASSERT_TRUE(waitFor([&releases]() { return !releases.empty(); }));

    const QUrlQuery& query = m_server->queries.back();
    EXPECT_EQ(u"I5l9cCSFccLKFEKS.7wqSZAorPU-"_s, query.queryItemValue(u"discId"_s));
    EXPECT_EQ(u"1+12+267257+150+22767+41887+58317+72102+91375+104652+115380+132165+143932+159870+174597"_s,
              query.queryItemValue(u"toc"_s, QUrl::FullyDecoded));

    const Subsonic::DiscRelease& release = releases.front();
    EXPECT_EQ(u"f1c2-release"_s, release.id);
    EXPECT_EQ(u"1999"_s, release.year);
    EXPECT_EQ(2, release.discNumber);
    ASSERT_EQ(2, release.tracks.size());
    EXPECT_EQ(2, release.tracks.at(1).disc);
    EXPECT_EQ(u"Two"_s, release.tracks.at(1).title);
    EXPECT_EQ(u"B"_s, release.tracks.at(1).artist);
}

TEST(SubsonicUploadTagsTest, StartsFromTheFilesTags)
{
    Track first{u"/music/a.flac"_s};
    first.setAlbum(u"Album"_s);
    first.setAlbumArtists({u"Artist"_s});
    first.setDate(u"2001-02-03"_s);
    first.setGenres({u"Rock"_s, u"Pop"_s});
    first.setTrackNumber(u"2"_s);
    first.setTitle(u"Second"_s);
    first.setArtists({u"A"_s, u"B"_s});
    Track second{u"/music/b.flac"_s};
    second.setTrackNumber(u"1"_s);

    const Subsonic::UploadTags tags = Subsonic::uploadTagsFromTracks({first, second});
    EXPECT_EQ(u"Album"_s, tags.album);
    EXPECT_EQ(u"Artist"_s, tags.albumArtist);
    EXPECT_EQ(u"2001"_s, tags.year);
    EXPECT_EQ(u"Rock; Pop"_s, tags.genre);
    ASSERT_EQ(2, tags.tracks.size());
    EXPECT_EQ(2, tags.tracks.at(0).number);
    EXPECT_EQ(u"A; B"_s, tags.tracks.at(0).artist);
    EXPECT_EQ(1, tags.tracks.at(1).number);
    // A file without a title is named after the file
    EXPECT_EQ(u"b"_s, tags.tracks.at(1).title);
    EXPECT_EQ(u"/music/b.flac"_s, tags.tracks.at(1).file);
}

TEST(SubsonicUploadTagsTest, NumbersTracksWhoseNumbersAreMissingOrRepeat)
{
    Track first{u"/music/a.flac"_s};
    first.setTrackNumber(u"1"_s);
    Track second{u"/music/b.flac"_s};
    second.setTrackNumber(u"1"_s);
    Track third{u"/music/c.flac"_s};

    const Subsonic::UploadTags tags = Subsonic::uploadTagsFromTracks({first, second, third});
    ASSERT_EQ(3, tags.tracks.size());
    for(int i{0}; i < 3; ++i) {
        EXPECT_EQ(1, tags.tracks.at(i).disc);
        EXPECT_EQ(i + 1, tags.tracks.at(i).number);
    }
}

TEST(SubsonicUploadTagsTest, AcceptsTheSuffixesOfTheExtension)
{
    for(const auto* suffix : {"flac", "FLAC", "opus", "mp3", "m4a", "ogg"}) {
        EXPECT_TRUE(Subsonic::isUploadSuffix(QString::fromLatin1(suffix)));
    }
    for(const auto* suffix : {"wav", "ape", "wv", ""}) {
        EXPECT_FALSE(Subsonic::isUploadSuffix(QString::fromLatin1(suffix)));
    }
}
} // namespace Fooyin::Testing

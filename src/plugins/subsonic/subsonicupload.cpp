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

#include "subsonicupload.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>

#include <set>

using namespace Qt::StringLiterals;

constexpr qint64 DefaultChunkSize = 4LL * 1024 * 1024;
//! Network failures in a row after which an upload stops and waits for the user.
constexpr auto MaxFailures        = 3;

namespace {
QString joined(const QStringList& values)
{
    return values.join(u"; "_s);
}

QJsonValue yearValue(const QString& year)
{
    bool ok{false};
    const int value = year.toInt(&ok);
    return ok ? QJsonValue{value} : QJsonValue{year};
}
} // namespace

namespace Fooyin::Subsonic {
bool isUploadSuffix(const QString& suffix)
{
    static const QStringList suffixes{u"flac"_s, u"opus"_s, u"mp3"_s, u"m4a"_s, u"ogg"_s};
    return suffixes.contains(suffix.toLower());
}

UploadTags uploadTagsFromTracks(const TrackList& tracks)
{
    UploadTags tags;
    std::set<std::pair<int, int>> slots;
    bool numbered{true};

    for(const Track& track : tracks) {
        if(tags.album.isEmpty()) {
            tags.album = track.album();
        }
        if(tags.albumArtist.isEmpty()) {
            tags.albumArtist = joined(track.albumArtists());
        }
        if(tags.year.isEmpty() && track.year() > 0) {
            tags.year = QString::number(track.year());
        }
        if(tags.genre.isEmpty()) {
            tags.genre = joined(track.genres());
        }

        const int disc   = std::max(1, track.discNumber().toInt());
        const int number = track.trackNumber().toInt();
        numbered         = numbered && number > 0 && slots.emplace(disc, number).second;

        tags.tracks.push_back({.disc   = disc,
                               .number = number,
                               .title  = track.title().isEmpty() ? QFileInfo{track.filepath()}.completeBaseName()
                                                                 : track.title(),
                               .artists = track.artists(),
                               .file   = track.filepath()});
    }

    if(!numbered) {
        int number{0};
        for(UploadTrack& track : tags.tracks) {
            track.disc   = 1;
            track.number = ++number;
        }
    }

    return tags;
}

QUrlQuery createUploadQuery(const UploadTags& tags)
{
    QUrlQuery query;
    if(!tags.releaseId.isEmpty()) {
        query.addQueryItem(u"releaseId"_s, tags.releaseId);
        query.addQueryItem(u"disc"_s, QString::number(tags.releaseDisc));
        return query;
    }

    QJsonArray tracks;
    for(const UploadTrack& track : tags.tracks) {
        QJsonObject object{{u"disc"_s, track.disc}, {u"number"_s, track.number}, {u"title"_s, track.title}};
        if(!track.artists.isEmpty()) {
            object.insert(u"artists"_s, QJsonArray::fromStringList(track.artists));
        }
        tracks.append(object);
    }

    QJsonObject object{{u"album"_s, tags.album}, {u"genre"_s, tags.genre}, {u"track"_s, tracks}};
    // The server takes the tracks' artists for an album without one
    if(!tags.albumArtist.isEmpty()) {
        object.insert(u"albumArtist"_s, tags.albumArtist);
    }
    if(!tags.year.isEmpty()) {
        object.insert(u"year"_s, yearValue(tags.year));
    }

    query.addQueryItem(u"tags"_s, QString::fromUtf8(QJsonDocument{object}.toJson(QJsonDocument::Compact)));
    return query;
}

QUrlQuery lookupDiscQuery(const QString& discId, const QString& toc)
{
    QUrlQuery query;
    query.addQueryItem(u"discId"_s, discId);
    query.addQueryItem(u"toc"_s, toc);
    return query;
}

std::vector<DiscRelease> discReleases(const QJsonObject& response)
{
    std::vector<DiscRelease> releases;
    for(const auto& item : toArray(response.value("discLookup"_L1).toObject().value("release"_L1))) {
        const QJsonObject object = item.toObject();

        DiscRelease release{.id            = object.value("id"_L1).toString(),
                            .title         = object.value("title"_L1).toString(),
                            .artist        = object.value("artist"_L1).toString(),
                            .year          = {},
                            .country       = object.value("country"_L1).toString(),
                            .label         = object.value("label"_L1).toString(),
                            .catalogNumber = object.value("catalogNumber"_L1).toString(),
                            .discCount     = std::max(1, object.value("discCount"_L1).toInt(1)),
                            .discNumber    = std::max(1, object.value("discNumber"_L1).toInt(1)),
                            .tracks        = {}};
        if(const int year = object.value("year"_L1).toInt(); year > 0) {
            release.year = QString::number(year);
        }
        else {
            release.year = object.value("date"_L1).toString().left(4);
        }

        for(const auto& trackItem : toArray(object.value("track"_L1))) {
            const QJsonObject track = trackItem.toObject();
            const QString artist = track.value("artist"_L1).toString();
            release.tracks.push_back({.disc    = release.discNumber,
                                      .number  = track.value("number"_L1).toInt(),
                                      .title   = track.value("title"_L1).toString(),
                                      .artists = artist.isEmpty() ? QStringList{} : QStringList{artist},
                                      .file    = {}});
        }

        if(!release.id.isEmpty()) {
            releases.push_back(std::move(release));
        }
    }
    return releases;
}

bool isDuplicateError(const ApiError& error)
{
    return error.duplicate;
}

Uploader::Uploader(QNetworkAccessManager* network, Account account, UploadTags tags, QObject* parent)
    : QObject{parent}
    , m_network{network}
    , m_account{std::move(account)}
    , m_tags{std::move(tags)}
    , m_chunkSize{DefaultChunkSize}
{ }

Uploader::~Uploader()
{
    m_cancelled = true;
    if(m_reply) {
        m_reply->abort();
    }
    removeWorkFolder();
}

void Uploader::setChunkSize(qint64 size)
{
    m_chunkSize = std::max<qint64>(1, size);
}

void Uploader::setWorkFolder(const QString& folder)
{
    m_workFolder = folder;
}

void Uploader::start()
{
    m_failures = 0;
    if(m_uploadId.isEmpty()) {
        createUpload();
    }
    else {
        // The server may have dropped a file since, for example one that failed its format check
        sync();
    }
}

void Uploader::replace()
{
    commit(true);
}

void Uploader::cancel()
{
    m_cancelled = true;
    if(m_reply) {
        m_reply->abort();
    }

    if(m_uploadId.isEmpty()) {
        removeWorkFolder();
        Q_EMIT cancelled();
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"uploadId"_s, m_uploadId);
    request(m_network, m_account, u"cancelUpload"_s, params, this, [this](std::optional<QJsonObject>, ApiError) {
        // The server deletes an abandoned upload after a day, so a failed cancel needs nothing more
        removeWorkFolder();
        Q_EMIT cancelled();
    });
}

QString Uploader::uploadId() const
{
    return m_uploadId;
}

void Uploader::createUpload()
{
    m_reply = request(
        m_network, m_account, u"createUpload"_s, createUploadQuery(m_tags), this,
        [this](std::optional<QJsonObject> response, const ApiError& error) {
            if(m_cancelled) {
                return;
            }
            if(!response) {
                fail(error);
                return;
            }

            const QJsonObject upload = response->value("upload"_L1).toObject();
            const QString uploadId   = upload.value("id"_L1).toString();
            if(uploadId.isEmpty()) {
                fail({.message = tr("The server did not create the upload.")});
                return;
            }

            std::vector<Slot> slots;
            for(const auto& item : toArray(upload.value("track"_L1))) {
                const QJsonObject slot = item.toObject();
                const int disc         = slot.value("disc"_L1).toInt(1);
                const int number       = slot.value("number"_L1).toInt();
                const auto track       = std::ranges::find_if(m_tags.tracks, [disc, number](const UploadTrack& t) {
                    return t.disc == disc && t.number == number;
                });
                if(track == m_tags.tracks.cend()) {
                    fail({.message = tr("The server expects disc %1, track %2, which is not in this upload.")
                                         .arg(disc)
                                         .arg(number)});
                    return;
                }
                slots.push_back({.disc = disc, .number = number, .file = track->file, .size = 0, .received = 0});
            }
            if(slots.empty()) {
                fail({.message = tr("The server expects no tracks for this upload.")});
                return;
            }

            for(Slot& slot : slots) {
                const QFileInfo info{slot.file};
                if(!info.isFile()) {
                    fail({.message = tr("The file %1 cannot be read.").arg(slot.file)});
                    return;
                }
                slot.size = info.size();
            }

            m_uploadId = uploadId;
            m_slots    = std::move(slots);
            sendNext();
        });
}

void Uploader::sendNext()
{
    if(m_cancelled) {
        return;
    }

    const auto slot = std::ranges::find_if(m_slots, [](const Slot& s) { return s.received < s.size; });
    if(slot == m_slots.end()) {
        commit(false);
        return;
    }

    QFile file{slot->file};
    if(!file.open(QIODevice::ReadOnly) || !file.seek(slot->received)) {
        fail({.message = tr("The file %1 cannot be read.").arg(slot->file)});
        return;
    }
    const QByteArray chunk = file.read(m_chunkSize);
    if(chunk.isEmpty()) {
        fail({.message = tr("The file %1 cannot be read.").arg(slot->file)});
        return;
    }

    QUrlQuery params;
    params.addQueryItem(u"uploadId"_s, m_uploadId);
    params.addQueryItem(u"disc"_s, QString::number(slot->disc));
    params.addQueryItem(u"number"_s, QString::number(slot->number));
    params.addQueryItem(u"offset"_s, QString::number(slot->received));
    params.addQueryItem(u"size"_s, QString::number(slot->size));
    params.addQueryItem(u"suffix"_s, QFileInfo{slot->file}.suffix().toLower());

    const qint64 sentBefore = sentBytes();
    const auto index        = static_cast<size_t>(std::distance(m_slots.begin(), slot));

    m_reply = post(m_network, m_account, u"uploadTrack"_s, params, chunk, this,
                   [this, index](std::optional<QJsonObject> response, const ApiError& error) {
                       if(m_cancelled) {
                           return;
                       }
                       Slot& current = m_slots.at(index);
                       if(!response) {
                           // Network failures resume from what the server stored; API errors stop the upload
                           if(error.code < 0 && ++m_failures < MaxFailures) {
                               sync();
                           }
                           else {
                               fail(error);
                           }
                           return;
                       }

                       const auto received
                           = static_cast<qint64>(response->value("uploadTrack"_L1).toObject().value("received"_L1).toDouble(-1));
                       if(received <= current.received || received > current.size) {
                           fail({.message = tr("The server stored an unexpected part of %1.").arg(current.file)});
                           return;
                       }

                       m_failures       = 0;
                       current.received = received;
                       Q_EMIT progress(sentBytes(), totalBytes());
                       sendNext();
                   });
    QObject::connect(m_reply, &QNetworkReply::uploadProgress, this, [this, sentBefore](qint64 sent, qint64) {
        Q_EMIT progress(sentBefore + sent, totalBytes());
    });
}

void Uploader::sync()
{
    QUrlQuery params;
    params.addQueryItem(u"id"_s, m_uploadId);

    m_reply = request(m_network, m_account, u"getUpload"_s, params, this,
                      [this](std::optional<QJsonObject> response, const ApiError& error) {
                          if(m_cancelled) {
                              return;
                          }
                          if(!response) {
                              if(error.code < 0 && ++m_failures < MaxFailures) {
                                  sync();
                              }
                              else {
                                  fail(error);
                              }
                              return;
                          }

                          for(const auto& item : toArray(response->value("upload"_L1).toObject().value("track"_L1))) {
                              const QJsonObject track = item.toObject();
                              const auto slot         = std::ranges::find_if(m_slots, [&track](const Slot& s) {
                                  return track.value("disc"_L1).toInt(1) == s.disc
                                      && track.value("number"_L1).toInt() == s.number;
                              });
                              if(slot != m_slots.end()) {
                                  slot->received = std::clamp(static_cast<qint64>(track.value("received"_L1).toDouble()),
                                                              qint64{0}, slot->size);
                              }
                          }
                          Q_EMIT progress(sentBytes(), totalBytes());
                          sendNext();
                      });
}

void Uploader::commit(bool replace)
{
    QUrlQuery params;
    params.addQueryItem(u"uploadId"_s, m_uploadId);
    if(replace) {
        params.addQueryItem(u"replace"_s, u"true"_s);
    }

    m_reply = request(m_network, m_account, u"commitUpload"_s, params, this,
                      [this](std::optional<QJsonObject> response, const ApiError& error) {
                          if(m_cancelled) {
                              return;
                          }
                          if(!response) {
                              fail(error);
                              return;
                          }
                          removeWorkFolder();
                          Q_EMIT committed(response->value("album"_L1).toObject());
                      });
}

void Uploader::fail(const ApiError& error)
{
    m_failures = 0;
    Q_EMIT failed(error);
}

void Uploader::removeWorkFolder()
{
    if(!m_workFolder.isEmpty()) {
        QDir{m_workFolder}.removeRecursively();
        m_workFolder.clear();
    }
}

qint64 Uploader::sentBytes() const
{
    qint64 sent{0};
    for(const Slot& slot : m_slots) {
        sent += slot.received;
    }
    return sent;
}

qint64 Uploader::totalBytes() const
{
    qint64 total{0};
    for(const Slot& slot : m_slots) {
        total += slot.size;
    }
    return total;
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicupload.cpp"

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

#include "subsonicapi.h"

#include <QObject>
#include <QPointer>

#include <vector>

namespace Fooyin::Subsonic {
//! A track of an upload: its slot on the server, its tags, and the local file sent for it.
struct UploadTrack
{
    int disc{1};
    int number{0};
    QString title;
    QString artist;
    QString file;
};

/*!
 * What `createUpload` gets (OpenSubsonic extension `upload` v1): a MusicBrainz release, from which the server takes
 * all tags, or the album fields and track tags.
 */
struct UploadTags
{
    QString releaseId;
    //! The disc of the release.
    int releaseDisc{1};
    QString album;
    QString albumArtist;
    QString year;
    QString genre;
    std::vector<UploadTrack> tracks;
};

//! A MusicBrainz release that `lookupDisc` found for a disc.
struct DiscRelease
{
    QString id;
    QString title;
    QString artist;
    QString year;
    QString country;
    QString label;
    QString catalogNumber;
    int discCount{1};
    //! The disc of the release that the TOC matched.
    int discNumber{1};
    //! Track slots on that disc, with their titles and artists.
    std::vector<UploadTrack> tracks;
};

//! True if the `upload` extension accepts files with @p suffix.
[[nodiscard]] bool isUploadSuffix(const QString& suffix);
/*!
 * Returns the starting tags of an upload of @p tracks, read from local files: the album fields of the first track
 * that has them, and each track's slot, title and artists. Tracks are numbered 1 to n on disc 1 when their own
 * numbers are missing or repeat.
 */
[[nodiscard]] UploadTags uploadTagsFromTracks(const TrackList& tracks);

//! The parameters of `createUpload` for @p tags.
[[nodiscard]] QUrlQuery createUploadQuery(const UploadTags& tags);
//! The parameters of `lookupDisc` for a disc's MusicBrainz disc id and TOC string.
[[nodiscard]] QUrlQuery lookupDiscQuery(const QString& discId, const QString& toc);
//! Reads the releases of a `lookupDisc` response, best match first.
[[nodiscard]] std::vector<DiscRelease> discReleases(const QJsonObject& response);
//! True if @p error is commitUpload's "already in the library", the one refusal that `replace=true` overrides.
[[nodiscard]] bool isDuplicateError(const ApiError& error);

/*!
 * Sends an upload to a server: `createUpload`, then each file in chunks through `uploadTrack`, then `commitUpload`.
 * A chunk that fails on the network is resumed from the size `getUpload` reports, up to three times in a row. After
 * failed(), start() asks `getUpload` again and continues from what the server kept. A work folder set with setWorkFolder() is deleted once the
 * upload is committed or cancelled, or the uploader is destroyed.
 */
class Uploader : public QObject
{
    Q_OBJECT

public:
    Uploader(QNetworkAccessManager* network, Account account, UploadTags tags, QObject* parent = nullptr);
    ~Uploader() override;

    void setChunkSize(qint64 size);
    void setWorkFolder(const QString& folder);

    //! Creates the upload, or continues it after failed().
    void start();
    //! Commits again with `replace=true`, after failed() with a duplicate error.
    void replace();
    //! Stops sending and deletes the upload on the server.
    void cancel();

    [[nodiscard]] QString uploadId() const;

Q_SIGNALS:
    void progress(qint64 sent, qint64 total);
    void failed(const Fooyin::Subsonic::ApiError& error);
    //! The new album (`AlbumID3` with `song[]`).
    void committed(const QJsonObject& album);
    void cancelled();

private:
    struct Slot
    {
        int disc{1};
        int number{0};
        QString file;
        qint64 size{0};
        qint64 received{0};
    };

    void createUpload();
    void sendNext();
    //! Asks getUpload what the server stored, and continues from there.
    void sync();
    void commit(bool replace);
    void fail(const ApiError& error);
    void removeWorkFolder();
    [[nodiscard]] qint64 sentBytes() const;
    [[nodiscard]] qint64 totalBytes() const;

    QNetworkAccessManager* m_network;
    Account m_account;
    UploadTags m_tags;
    qint64 m_chunkSize;
    QString m_workFolder;
    QString m_uploadId;
    std::vector<Slot> m_slots;
    QPointer<QNetworkReply> m_reply;
    int m_failures{0};
    bool m_cancelled{false};
};
} // namespace Fooyin::Subsonic

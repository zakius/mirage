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

#include "subsonicupload.h"

#include <core/track.h>

#include <QDialog>
#include <QPointer>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace Fooyin {
class AudioLoader;
class NetworkAccessManager;

namespace Cdda {
class DiscRipper;
}

namespace Subsonic {
class SubsonicProvider;

/*!
 * Uploads local files, or an audio CD it rips, to a server with the `upload` extension. The user picks the server
 * and edits the album and track tags, or for a CD picks a MusicBrainz release the server found. Files are read only
 * here; a CD is ripped to a folder in mirage's cache, which is deleted after the upload is committed or cancelled.
 */
class UploadDialog : public QDialog
{
    Q_OBJECT

public:
    struct Server
    {
        QString libraryPath;
        QString name;
    };

    UploadDialog(SubsonicProvider* provider, std::shared_ptr<NetworkAccessManager> network,
                 const std::vector<Server>& servers, const QString& server, QWidget* parent = nullptr);
    ~UploadDialog() override;

    //! Uploads local files, whose tags @p audioLoader reads.
    void setAudioLoader(std::shared_ptr<AudioLoader> audioLoader);
    //! Rips and uploads the CD with @p tracks, from DiscRipper::chooseDisc().
    void setDisc(Cdda::DiscRipper* ripper, const TrackList& tracks);

    void reject() override;

Q_SIGNALS:
    //! Emitted when an upload to the server of @p libraryPath has been committed.
    void uploaded(const QString& libraryPath);

private:
    enum class State : uint8_t
    {
        Editing,
        Ripping,
        Uploading,
        Failed,
        Done,
    };

    void addFiles();
    void addFolder();
    void readFiles(const QStringList& files, int skipped);
    void addTracks(const UploadTags& tags);

    void lookupDisc();
    void releaseChanged();

    [[nodiscard]] QString libraryPath() const;
    [[nodiscard]] std::optional<UploadTags> tagsFromForm();
    void startUpload();
    void rip(UploadTags tags);
    void send(UploadTags tags);
    void uploadFailed(const ApiError& error);

    void setState(State state);
    void setStatus(const QString& text);

    SubsonicProvider* m_provider;
    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<AudioLoader> m_audioLoader;
    Cdda::DiscRipper* m_ripper{nullptr};
    TrackList m_discTracks;
    std::vector<DiscRelease> m_releases;
    QString m_workFolder;
    QPointer<Uploader> m_uploader;
    State m_state{State::Editing};

    QComboBox* m_server;
    QLabel* m_releaseLabel;
    QComboBox* m_release;
    QLineEdit* m_album;
    QLineEdit* m_albumArtist;
    QLineEdit* m_year;
    QLineEdit* m_genre;
    QWidget* m_fileButtons;
    QTableWidget* m_tracks;
    QLabel* m_status;
    QProgressBar* m_progress;
    QDialogButtonBox* m_buttons;
    QPushButton* m_upload;
    QPushButton* m_replace;
};
} // namespace Subsonic
} // namespace Fooyin

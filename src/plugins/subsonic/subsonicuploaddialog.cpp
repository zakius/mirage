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

#include "subsonicuploaddialog.h"

#include "subsonicprovider.h"

#include "../cdda/cddadiscripper.h"

#include <core/engine/audioloader.h>
#include <core/network/networkaccessmanager.h>
#include <utils/async.h>
#include <utils/fypaths.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <set>

using namespace Qt::StringLiterals;

namespace {
enum Column : uint8_t
{
    Disc = 0,
    Number,
    Title,
    Artist,
    File,
};

//! The folder in mirage's cache that holds ripped CDs until they are uploaded.
QString uploadsFolder()
{
    return Fooyin::Utils::cachePath(u"uploads"_s);
}

QString releaseName(const Fooyin::Subsonic::DiscRelease& release)
{
    QStringList details;
    for(const QString& detail : {release.year, release.country, release.label, release.catalogNumber}) {
        if(!detail.isEmpty()) {
            details.append(detail);
        }
    }
    if(release.discCount > 1) {
        details.append(Fooyin::Subsonic::UploadDialog::tr("disc %1 of %2").arg(release.discNumber).arg(release.discCount));
    }

    QString name = u"%1 – %2"_s.arg(release.artist, release.title);
    if(!details.isEmpty()) {
        name += u" ("_s + details.join(u", "_s) + u")"_s;
    }
    return name;
}
} // namespace

namespace Fooyin::Subsonic {
UploadDialog::UploadDialog(SubsonicProvider* provider, std::shared_ptr<NetworkAccessManager> network,
                           const std::vector<Server>& servers, const QString& server, QWidget* parent)
    : QDialog{parent}
    , m_provider{provider}
    , m_network{std::move(network)}
    , m_server{new QComboBox(this)}
    , m_releaseLabel{new QLabel(tr("Release") + u":"_s, this)}
    , m_release{new QComboBox(this)}
    , m_album{new QLineEdit(this)}
    , m_albumArtist{new QLineEdit(this)}
    , m_year{new QLineEdit(this)}
    , m_genre{new QLineEdit(this)}
    , m_fileButtons{new QWidget(this)}
    , m_tracks{new QTableWidget(0, 5, this)}
    , m_status{new QLabel(this)}
    , m_progress{new QProgressBar(this)}
    , m_buttons{new QDialogButtonBox(QDialogButtonBox::Close, this)}
    , m_upload{m_buttons->addButton(tr("&Upload"), QDialogButtonBox::AcceptRole)}
    , m_replace{m_buttons->addButton(tr("&Replace"), QDialogButtonBox::ActionRole)}
{
    setWindowTitle(tr("Upload to Server"));
    resize(820, 560);

    for(const Server& entry : servers) {
        m_server->addItem(entry.name, entry.libraryPath);
    }
    m_server->setCurrentIndex(std::max(0, m_server->findData(server)));

    m_release->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_releaseLabel->hide();
    m_release->hide();

    auto* form = new QFormLayout();
    form->addRow(tr("Server") + u":"_s, m_server);
    form->addRow(m_releaseLabel, m_release);
    form->addRow(tr("Album") + u":"_s, m_album);
    form->addRow(tr("Album artist") + u":"_s, m_albumArtist);
    form->addRow(tr("Year") + u":"_s, m_year);
    form->addRow(tr("Genre") + u":"_s, m_genre);

    auto* addFiles  = new QPushButton(tr("Add &Files…"), m_fileButtons);
    auto* addFolder = new QPushButton(tr("Add F&older…"), m_fileButtons);
    auto* remove    = new QPushButton(tr("Re&move"), m_fileButtons);
    auto* fileRow   = new QHBoxLayout(m_fileButtons);
    fileRow->setContentsMargins(0, 0, 0, 0);
    fileRow->addWidget(addFiles);
    fileRow->addWidget(addFolder);
    fileRow->addWidget(remove);
    fileRow->addStretch();
    m_fileButtons->hide();

    m_tracks->setHorizontalHeaderLabels({tr("Disc"), tr("Track"), tr("Title"), tr("Artist"), tr("File")});
    m_tracks->verticalHeader()->hide();
    m_tracks->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tracks->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tracks->horizontalHeader()->setSectionResizeMode(Title, QHeaderView::Stretch);

    m_status->setWordWrap(true);
    m_progress->hide();
    m_replace->hide();

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_fileButtons);
    layout->addWidget(m_tracks, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_buttons);

    QObject::connect(addFiles, &QPushButton::clicked, this, &UploadDialog::addFiles);
    QObject::connect(addFolder, &QPushButton::clicked, this, &UploadDialog::addFolder);
    QObject::connect(remove, &QPushButton::clicked, this, [this]() {
        std::set<int> rows;
        for(const QModelIndex& index : m_tracks->selectionModel()->selectedRows()) {
            rows.emplace(index.row());
        }
        for(auto row = rows.rbegin(); row != rows.rend(); ++row) {
            m_tracks->removeRow(*row);
        }
    });
    QObject::connect(m_server, &QComboBox::currentIndexChanged, this, [this]() {
        if(m_ripper) {
            lookupDisc();
        }
    });
    QObject::connect(m_release, &QComboBox::currentIndexChanged, this, &UploadDialog::releaseChanged);
    QObject::connect(m_upload, &QPushButton::clicked, this, &UploadDialog::startUpload);
    QObject::connect(m_replace, &QPushButton::clicked, this, [this]() {
        if(m_uploader) {
            setState(State::Uploading);
            setStatus(tr("Replacing the server's copy…"));
            m_uploader->replace();
        }
    });
    QObject::connect(m_buttons, &QDialogButtonBox::rejected, this, &UploadDialog::reject);
}

UploadDialog::~UploadDialog()
{
    if(!m_workFolder.isEmpty()) {
        QDir{m_workFolder}.removeRecursively();
    }
}

void UploadDialog::setAudioLoader(std::shared_ptr<AudioLoader> audioLoader)
{
    m_audioLoader = std::move(audioLoader);
    m_fileButtons->show();
    setStatus(tr("Add the files of an album, or single tracks. The server accepts FLAC, Opus, MP3, M4A and Ogg."));
}

void UploadDialog::setDisc(Cdda::DiscRipper* ripper, const TrackList& tracks)
{
    setWindowTitle(tr("Rip CD to Server"));
    m_ripper     = ripper;
    m_discTracks = tracks;
    m_tracks->setColumnHidden(File, true);
    m_releaseLabel->show();
    m_release->show();

    addTracks(uploadTagsFromTracks(tracks));
    lookupDisc();
}

void UploadDialog::reject()
{
    if(m_state == State::Ripping) {
        setStatus(tr("Cancel the rip in its progress window."));
        return;
    }
    if(m_uploader && m_state != State::Done) {
        setStatus(tr("Cancelling the upload…"));
        m_buttons->setEnabled(false);
        QObject::connect(m_uploader, &Uploader::cancelled, this, [this]() { QDialog::reject(); });
        m_uploader->cancel();
        return;
    }
    QDialog::reject();
}

void UploadDialog::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add Files"), QDir::homePath(), tr("Audio files") + u" (*.flac *.opus *.mp3 *.m4a *.ogg)"_s);
    if(!files.isEmpty()) {
        readFiles(files, 0);
    }
}

void UploadDialog::addFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add Folder"), QDir::homePath());
    if(folder.isEmpty()) {
        return;
    }

    QStringList files;
    int skipped{0};
    QDirIterator it{folder, QDir::Files, QDirIterator::Subdirectories};
    while(it.hasNext()) {
        const QFileInfo info{it.next()};
        if(isUploadSuffix(info.suffix())) {
            files.append(info.filePath());
        }
        else if(info.suffix().compare("jpg"_L1, Qt::CaseInsensitive) != 0
                && info.suffix().compare("png"_L1, Qt::CaseInsensitive) != 0) {
            ++skipped;
        }
    }
    files.sort();

    if(files.isEmpty()) {
        setStatus(tr("The folder has no FLAC, Opus, MP3, M4A or Ogg files."));
        return;
    }
    readFiles(files, skipped);
}

void UploadDialog::readFiles(const QStringList& files, int skipped)
{
    setStatus(tr("Reading tags…"));
    m_upload->setEnabled(false);

    Utils::asyncExec([audioLoader = m_audioLoader, files]() {
        TrackList tracks;
        for(const QString& file : files) {
            Track track{file};
            std::ignore = audioLoader->readTrackMetadata(track);
            tracks.push_back(std::move(track));
        }
        return tracks;
    }).then(this, [this, skipped](const TrackList& tracks) {
        addTracks(uploadTagsFromTracks(tracks));
        m_upload->setEnabled(true);
        setStatus(skipped > 0 ? tr("Skipped %n file(s) that the server does not accept.", nullptr, skipped)
                              : QString{});
    });
}

void UploadDialog::addTracks(const UploadTags& tags)
{
    const auto fill = [](QLineEdit* edit, const QString& value) {
        if(edit->text().isEmpty()) {
            edit->setText(value);
        }
    };
    fill(m_album, tags.album);
    fill(m_albumArtist, tags.albumArtist);
    fill(m_year, tags.year);
    fill(m_genre, tags.genre);

    for(const UploadTrack& track : tags.tracks) {
        const int row = m_tracks->rowCount();
        m_tracks->insertRow(row);
        m_tracks->setItem(row, Disc, new QTableWidgetItem(QString::number(track.disc)));
        m_tracks->setItem(row, Number, new QTableWidgetItem(QString::number(track.number)));
        m_tracks->setItem(row, Title, new QTableWidgetItem(track.title));
        m_tracks->setItem(row, Artist, new QTableWidgetItem(track.artist));
        auto* file = new QTableWidgetItem(QFileInfo{track.file}.fileName());
        file->setData(Qt::UserRole, track.file);
        file->setFlags(file->flags() & ~Qt::ItemIsEditable);
        file->setToolTip(track.file);
        m_tracks->setItem(row, File, file);
    }
}

void UploadDialog::lookupDisc()
{
    const QSignalBlocker blocker{m_release};
    m_releases.clear();
    m_release->clear();
    m_release->addItem(tr("Enter the tags myself"));
    releaseChanged();

    if(m_discTracks.empty()) {
        return;
    }

    const QString discId = Cdda::discProperty(m_discTracks.front(), Cdda::DiscIdProperty);
    const QString toc    = Cdda::discProperty(m_discTracks.front(), Cdda::DiscTocProperty);
    setStatus(tr("Looking up the disc…"));

    const QString server = libraryPath();
    m_provider->call(server, u"lookupDisc"_s, lookupDiscQuery(discId, toc),
                     [this, self = QPointer{this}, server](std::optional<QJsonObject> response, const ApiError& error) {
                         if(!self || server != libraryPath() || m_state != State::Editing) {
                             return;
                         }
                         if(!response) {
                             setStatus(tr("The disc lookup failed: %1. Enter the tags yourself.").arg(error.message));
                             return;
                         }

                         m_releases = discReleases(*response);
                         if(m_releases.empty()) {
                             setStatus(tr("The server found no release for this disc. Enter the tags yourself."));
                             return;
                         }

                         const QSignalBlocker releaseBlocker{m_release};
                         for(int i{0}; std::cmp_less(i, m_releases.size()); ++i) {
                             m_release->insertItem(i, releaseName(m_releases.at(i)));
                         }
                         m_release->setCurrentIndex(0);
                         releaseChanged();
                         setStatus(tr("The server takes the tags of the chosen release from MusicBrainz. To edit "
                                      "them, choose “Enter the tags myself”."));
                     });
}

void UploadDialog::releaseChanged()
{
    const int index      = m_release->currentIndex();
    const bool ownTags   = index < 0 || std::cmp_greater_equal(index, m_releases.size());
    const auto setEditable = [ownTags](QLineEdit* edit) {
        edit->setReadOnly(!ownTags);
    };
    setEditable(m_album);
    setEditable(m_albumArtist);
    setEditable(m_year);
    m_genre->setEnabled(ownTags);
    m_tracks->setEditTriggers(ownTags ? QAbstractItemView::AllEditTriggers : QAbstractItemView::NoEditTriggers);

    if(ownTags) {
        return;
    }

    // The release's tags are shown, and stay as the starting point for "Enter the tags myself"
    const DiscRelease& release = m_releases.at(index);
    m_album->setText(release.title);
    m_albumArtist->setText(release.artist);
    m_year->setText(release.year);
    for(int row{0}; row < m_tracks->rowCount(); ++row) {
        const int number = m_tracks->item(row, Number)->text().toInt();
        m_tracks->item(row, Disc)->setText(QString::number(release.discNumber));
        const auto track = std::ranges::find(release.tracks, number, &UploadTrack::number);
        if(track != release.tracks.cend()) {
            m_tracks->item(row, Title)->setText(track->title);
            m_tracks->item(row, Artist)->setText(track->artist);
        }
    }
}

QString UploadDialog::libraryPath() const
{
    return m_server->currentData().toString();
}

std::optional<UploadTags> UploadDialog::tagsFromForm()
{
    UploadTags tags;
    tags.album       = m_album->text().trimmed();
    tags.albumArtist = m_albumArtist->text().trimmed();
    tags.year        = m_year->text().trimmed();
    tags.genre       = m_genre->text().trimmed();

    const int release = m_release->currentIndex();
    if(release >= 0 && std::cmp_less(release, m_releases.size())) {
        tags.releaseId   = m_releases.at(release).id;
        tags.releaseDisc = m_releases.at(release).discNumber;
    }

    std::set<std::pair<int, int>> slots;
    for(int row{0}; row < m_tracks->rowCount(); ++row) {
        const UploadTrack track{.disc   = m_tracks->item(row, Disc)->text().toInt(),
                                .number = m_tracks->item(row, Number)->text().toInt(),
                                .title  = m_tracks->item(row, Title)->text().trimmed(),
                                .artist = m_tracks->item(row, Artist)->text().trimmed(),
                                .file   = m_tracks->item(row, File)->data(Qt::UserRole).toString()};
        if(track.disc < 1 || track.number < 1) {
            setStatus(tr("Row %1 needs a disc and track number of 1 or more.").arg(row + 1));
            return {};
        }
        if(track.title.isEmpty()) {
            setStatus(tr("Row %1 needs a title.").arg(row + 1));
            return {};
        }
        if(!slots.emplace(track.disc, track.number).second) {
            setStatus(tr("Two rows are disc %1, track %2.").arg(track.disc).arg(track.number));
            return {};
        }
        tags.tracks.push_back(track);
    }

    if(tags.tracks.empty()) {
        setStatus(tr("Add the files to upload first."));
        return {};
    }
    // The server files an upload without an album as a single, named after its first track
    if(tags.releaseId.isEmpty() && tags.album.isEmpty() && tags.tracks.size() > 1) {
        setStatus(tr("Enter the album, or upload single tracks one at a time."));
        return {};
    }
    return tags;
}

void UploadDialog::startUpload()
{
    if(m_uploader) {
        // Continues after a failure, with the tags the upload was created with
        setState(State::Uploading);
        setStatus(tr("Uploading…"));
        m_uploader->start();
        return;
    }

    auto tags = tagsFromForm();
    if(!tags) {
        return;
    }
    if(m_ripper) {
        rip(std::move(*tags));
    }
    else {
        send(std::move(*tags));
    }
}

void UploadDialog::rip(UploadTags tags)
{
    m_workFolder = uploadsFolder() + u"/"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!QDir{}.mkpath(m_workFolder)) {
        setStatus(tr("Cannot create a folder for the rip in %1.").arg(uploadsFolder()));
        m_workFolder.clear();
        return;
    }

    setState(State::Ripping);
    setStatus(tr("Ripping the disc to FLAC…"));

    m_ripper->ripToFolder(m_discTracks, m_workFolder,
                          [this, self = QPointer{this}, tags = std::move(tags)](Cdda::DiscRipper::Rip rip) mutable {
                              if(!self) {
                                  return;
                              }
                              if(rip.cancelled || !rip.error.isEmpty()) {
                                  QDir{m_workFolder}.removeRecursively();
                                  m_workFolder.clear();
                                  setState(State::Editing);
                                  setStatus(rip.cancelled ? tr("The rip was cancelled.")
                                                          : tr("The rip failed: %1").arg(rip.error));
                                  return;
                              }
                              if(rip.accurateRipMismatch
                                 && QMessageBox::question(this, tr("AccurateRip"),
                                                          tr("AccurateRip found tracks that do not match other rips "
                                                             "of this disc. Upload them anyway?"))
                                        != QMessageBox::Yes) {
                                  QDir{m_workFolder}.removeRecursively();
                                  m_workFolder.clear();
                                  setState(State::Editing);
                                  setStatus(tr("Nothing was uploaded."));
                                  return;
                              }

                              for(size_t i{0}; i < tags.tracks.size() && std::cmp_less(i, rip.files.size()); ++i) {
                                  tags.tracks.at(i).file = rip.files.at(static_cast<qsizetype>(i));
                              }
                              send(std::move(tags));
                          });
}

void UploadDialog::send(UploadTags tags)
{
    const QString server = libraryPath();
    const auto account   = m_provider->accountForPath(server);
    if(!account) {
        setStatus(tr("The server's account is missing."));
        return;
    }

    m_uploader = new Uploader(m_network.get(), *account, std::move(tags), this);
    if(!m_workFolder.isEmpty()) {
        m_uploader->setWorkFolder(std::exchange(m_workFolder, {}));
    }

    QObject::connect(m_uploader, &Uploader::progress, this, [this](qint64 sent, qint64 total) {
        m_progress->setValue(total > 0 ? static_cast<int>(sent * 1000 / total) : 0);
    });
    QObject::connect(m_uploader, &Uploader::failed, this, &UploadDialog::uploadFailed);
    QObject::connect(m_uploader, &Uploader::committed, this, [this, server](const QJsonObject& album) {
        setState(State::Done);
        setStatus(tr("“%1” was added to the server. It appears in the library once it has been refreshed.")
                      .arg(album.value("name"_L1).toString()));
        Q_EMIT uploaded(server);
    });

    setState(State::Uploading);
    setStatus(tr("Uploading…"));
    m_uploader->start();
}

void UploadDialog::uploadFailed(const ApiError& error)
{
    setState(State::Failed);
    if(isDuplicateError(error)) {
        m_replace->show();
        setStatus(tr("The server says that this album is already in the library (%1). Replace the server's copy "
                     "with this upload, or close to cancel it.")
                      .arg(error.message));
        return;
    }
    setStatus(tr("The upload stopped: %1. Retry to continue where it stopped, or close to cancel it.")
                  .arg(error.message));
}

void UploadDialog::setState(State state)
{
    m_state          = state;
    const bool idle  = state == State::Editing;
    const bool busy  = state == State::Ripping || state == State::Uploading;

    m_server->setEnabled(idle);
    m_release->setEnabled(idle);
    m_fileButtons->setEnabled(idle);
    for(QLineEdit* edit : {m_album, m_albumArtist, m_year, m_genre}) {
        edit->setEnabled(idle);
    }
    m_tracks->setEnabled(idle);
    if(idle) {
        releaseChanged();
    }

    m_upload->setText(state == State::Failed ? tr("&Retry") : tr("&Upload"));
    m_upload->setVisible(state != State::Done);
    m_upload->setEnabled(!busy);
    m_replace->hide();
    m_buttons->button(QDialogButtonBox::Close)->setText(idle || state == State::Done ? tr("&Close") : tr("&Cancel"));
    m_progress->setRange(0, 1000);
    m_progress->setVisible(state == State::Uploading || state == State::Failed);
}

void UploadDialog::setStatus(const QString& text)
{
    m_status->setText(text);
}
} // namespace Fooyin::Subsonic

#include "moc_subsonicuploaddialog.cpp"

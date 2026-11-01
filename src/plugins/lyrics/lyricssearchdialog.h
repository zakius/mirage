/*
 * Fooyin
 * Copyright © 2026, Luke Taylor <luket@pm.me>
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

#include "lyrics.h"

#include <core/network/networkaccessmanager.h>
#include <core/track.h>

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QSplitter;
class QTreeView;

namespace Fooyin {
class LibraryProviderRegistry;
class LineEditEditor;
class SettingsManager;

namespace Lyrics {
class LyricsFinder;
class LyricsSearchModel;
class LyricsSaver;

class LyricsSearchDialog : public QDialog
{
    Q_OBJECT

public:
    LyricsSearchDialog(const Track& track, std::shared_ptr<NetworkAccessManager> networkAccess,
                       std::shared_ptr<LibraryProviderRegistry> libraryProviders, LyricsSaver* lyricsSaver,
                       SettingsManager* settings, QWidget* parent = nullptr);

    void accept() override;

    [[nodiscard]] QSize sizeHint() const override;

private:
    void search();
    void addResult(const Track& track, const Lyrics& lyrics);
    void finishSearch(const Track& track, bool foundAny);
    void updateSelection();
    void updateActionState();
    void saveState();
    void restoreState();
    bool applySelection();
    [[nodiscard]] int selectedSourceRow() const;
    [[nodiscard]] const Lyrics* selectedLyrics() const;
    [[nodiscard]] Lyrics editedSelectedLyrics() const;

    Track m_track;
    LyricsSaver* m_lyricsSaver;
    LyricsFinder* m_finder;

    LineEditEditor* m_titleEdit;
    LineEditEditor* m_albumEdit;
    LineEditEditor* m_artistEdit;
    QPushButton* m_searchButton;
    QLabel* m_statusLabel;
    QTreeView* m_resultsTable;
    LyricsSearchModel* m_resultsModel;
    QSortFilterProxyModel* m_resultsProxyModel;
    QPlainTextEdit* m_preview;
    QSplitter* m_splitter;
    QPushButton* m_okButton;
    QPushButton* m_applyButton;
};
} // namespace Lyrics
} // namespace Fooyin

# mirage: Subsonic libraries in fooyin

mirage is a fooyin fork that shows a remote Subsonic library as part of fooyin's own library. Server tracks appear in
the library tree, filters, search, columns and autoplaylists exactly like local files. There is no separate browser
panel and no "download to library" step.

The plugin talks to any Subsonic or OpenSubsonic server through the standard API and treats all servers the same.
Server-specific behaviour (for example oasis merging a local library with a streaming service) stays on the server
and is invisible to the client.

mirage is Subsonic only: its library consists of Subsonic servers, never folders, and its playlists live on the
server. Local files cannot be opened, added or dropped in, and fooyin's features for local files are hidden (see
"Local media"). It runs as "mirage" with its own configuration,
database and MPRIS name, so it does not touch a fooyin install on the same machine.

The work has two layers:

1. A small, generic **library provider** API in fooyin core. It lets a plugin own a library that is not a folder.
   It is kept on its own branch so it can be offered upstream.
2. A **Subsonic provider plugin** built on that API, in `src/plugins/subsonic`.

## What fooyin does today

- A library is a scanned folder: `LibraryInfo{name, path, id}`, stored in the `Libraries` table.
- `LibraryThreadHandler` queues scan requests. `LibraryScanner` runs them on a worker thread through
  `LibraryScanSession`, which enumerates files, reads tags, and writes batches through `LibraryScanWriter`. Tracks
  that are no longer found are written back with `libraryId = -1` and disabled. `UnifiedMusicLibrary` applies the
  results and emits `tracksAdded`, `tracksUpdated` and `tracksDeleted`, which every library view uses.
- Tracks are keyed by `FilePath`. Playback statistics live in `TrackStats`, keyed by the metadata hash.
- `Track::isVirtual()` is already true for any non-file, non-HTTP URI (for example `cdda://`). Virtual tracks count as
  existing, so "remove unavailable tracks" keeps them. Their metadata is database-only.
- `AudioLoader` opens HTTP(S) tracks through `RemoteSourceProvider` (implemented by `RemoteIoService`), and opens
  other schemes only through decoders that claim the scheme. `NetworkStreamDevice` is sequential, and FFmpeg treats
  `track.isRemote()` tracks as unseekable radio streams.
- `CoverRepository` downloads front covers for remote tracks from an `ARTWORKURL` extra tag through
  `RemoteIoService`.

## Library provider API (branch `library-providers`)

### Registration

```cpp
// include/core/library/libraryprovider.h
class FYCORE_EXPORT LibraryProvider
{
public:
    using FetchCallback = std::function<void(std::optional<TrackList> tracks)>;

    virtual ~LibraryProvider() = default;

    //! URL scheme owned by this provider, for example "subsonic".
    [[nodiscard]] virtual QString scheme() const = 0;
    //! Fetches the complete track list of `library`. Called and answered on the main thread.
    virtual void fetchTracks(const LibraryInfo& library, FetchCallback callback) = 0;
    //! Opens a track URL for playback. Called from engine threads.
    [[nodiscard]] virtual RemoteStreamSource createStreamSource(const QUrl& url) const = 0;
    //! Maps a provider URL (for example artwork) to a fetchable HTTP(S) URL. Called from any thread.
    [[nodiscard]] virtual QUrl resolveUrl(const QUrl& url) const = 0;
    //! True if the provider reports plays of `track` to its own service, so client-side scrobblers skip it.
    [[nodiscard]] virtual bool reportsPlays(const Track& track) const;
    //! Searches the server for tracks the library may not hold. The default finds nothing.
    virtual void searchTracks(const LibraryInfo& library, const QString& query, FetchCallback callback);
};
```

A thread-safe `LibraryProviderRegistry` (core, owned by `Application`) holds providers by scheme. Plugins reach it
through a new `CorePluginContext::libraryProviders` field.

A provider library is an ordinary `LibraryInfo` whose `path` is a URL with the provider's scheme, for example
`subsonic://alice@music.example.org:4533`. The plugin adds it with `LibraryManager::addLibrary()`. This needs no
schema change. Rename, remove, status and the library settings page keep working. Track paths are URLs under the
library path, so `libraryForPath()` also works.

### Getting tracks into the library

A scan or refresh of a provider library does not touch the filesystem:

1. `LibraryThreadHandler` sees that the library path has a registered scheme and calls `fetchTracks()` on the main
   thread.
2. The callback hands the complete list to a new `LibraryScanner::syncLibrary()` on the scanner thread.
3. `LibraryScanSession::syncLibrary()` compares the list with the existing tracks by `FilePath`. It stores new tracks,
   updates changed ones, and disables tracks that are gone, the same way a folder scan does. It writes through the
   existing `LibraryScanWriter`, so progress, `scanSummary`, `tracksAdded`, `tracksUpdated` and the library views work
   unchanged.

A provider sets only fields that the database stores, so a second sync of unchanged data writes nothing. This is the
incremental refresh: each sync fetches the whole index, and only real changes reach the database and the views.
`refresh()`, `rescan()`, "auto refresh on startup" and adding a library all go through this path. Times the provider
does not report (added, first played, last played) keep their stored values.

### Playback

`RemoteSourceProvider` gets `virtual bool handlesScheme(const QString& scheme) const`, which defaults to `false`.
`RemoteIoService` answers it from the registry and forwards `createStreamSource()` for provider schemes to the
provider. `AudioLoader` treats a virtual track whose scheme is handled this way as a remote source: it picks
remote-capable decoders (FFmpeg) and opens the device through the provider.

The provider returns the device, so the core needs no new HTTP code. The Subsonic plugin returns a seekable device
that uses HTTP range requests. The track itself is not `isRemote()`, so FFmpeg keeps seeking enabled.

### Artwork

`CoverRepository` already downloads `ARTWORKURL` for remote tracks. Two changes extend this to provider tracks:

- Accept artwork URLs whose scheme is a registered provider scheme, and treat provider tracks like remote tracks.
- `RemoteIoService::download()` passes provider URLs through `resolveUrl()` first.

The provider stores a credential-free URL such as `subsonic://alice@host:4533/cover/al-123` in the tag. The resolved
URL with the auth token never reaches the database.

### Stats, ratings and stars

No core change. Provider tracks carry server play counts, ratings and stars in the track list, and sync stores them.
The server is the source of truth. The plugin listens to `MusicLibrary::tracksStatsChanged` and writes rating and
love changes back to the server. Sync results arrive through `tracksUpdated`, not `tracksStatsChanged`, so they do not
echo back.

`TrackStats` is keyed by the metadata hash. A local copy of a server track therefore shares its statistics, and a sync
writes the server values over them. This is intended.

### Missing-file cleanup and rescans

- Provider tracks are virtual, so `Track::exists()` is true. "Remove unavailable tracks" used its own file check and
  deleted all remote and virtual tracks; it now uses `Track::exists()`.
- A rescan of a provider library goes to the provider. A track disappears only when the server stops listing it.
- `LibraryMonitor` skips provider libraries, because they have no directory to watch.
- "Reload tags" (`scanTracks`) skips provider tracks, because there is no file to read.

### Scrobbling

The scrobbler plugin skips tracks for which their provider's `reportsPlays(track)` returns `true`. The answer is per
track, so a provider can decide per server whether its own service already forwards the plays (for example to
Last.fm), and Last.fm does not count them twice.

### Server search

`MusicLibrary::searchProviders()` asks every provider library's `searchTracks()` and returns the found tracks that the
library does not hold (matched by path), once. The "Search Library" dialog (Library > Search, fooyin's library search
that lists tracks) calls it for queries of three or more characters that are not scripts (no `%`), 400 ms after typing
stops, and appends the results after the local matches. Results for an older query are dropped. The library tree and
the filters show only library tracks, so they are not extended.

## Subsonic provider plugin (branch `mirage`, `src/plugins/subsonic`)

foonic (iljasrb/foonic) has no license, so none of its code is reused. The client is written from scratch against the
Subsonic API documentation.

- **Client.** Token authentication (`t = md5(password + salt)`, `s = salt`), `f=json`, `c=mirage`, `v=1.16.1`. Errors
  are taken from `subsonic-response.error`.
- **Account.** A settings page adds a server: base URL, user name, password. The password goes to the core
  `AccountStore`. The page calls `addLibrary("subsonic://user@host:port[/path]", name)`.
- **Full sync.** `search3` with `query=""`, `songCount=500`, paged by `songOffset` until a short page arrives. Each
  song becomes a `Track`:
  - path `<library path>/song/<id>`
  - title, artists (OpenSubsonic `artists` if present), album, album artist (`displayAlbumArtist` or `albumArtists`)
  - track and disc number, year, genres, duration, bit rate, size, codec from `suffix`
  - `created` as the added time, `playCount`, `played` as the last-played time, `userRating` as stars, `starred` as
    loved
  - an `ARTWORKURL` tag with `<library path>/cover/<coverArt>`
  - composers and performers from OpenSubsonic `contributors` (`displayComposer` if there are no composers), and the
    comment
  - extra tags for fields without a track field, set only where the server sends them: `SUFFIX`, `CONTENTTYPE`,
    `SERVERPATH` (`path`), `AVERAGERATING`, `TITLESORT` (`sortName`), `DISPLAYARTIST`, `DISPLAYALBUMARTIST`, `MOOD`,
    `BPM`, `MUSICBRAINZ_TRACKID`, `ISRC`, `EXPLICITSTATUS`, `WORK` (`works`), `MOVEMENT` (`movements`),
    `REPLAYGAIN_BASE_GAIN`, `REPLAYGAIN_FALLBACK_GAIN`, and one tag per other contributor role (for example
    `PRODUCER`), with the sub-role in parentheses

  If a server rejects an empty `search3` query (error 10) or returns no songs for it, the plugin falls back to walking
  `getAlbumList2` (`alphabeticalByName`) and `getAlbum`. Other errors, such as wrong credentials, fail the sync and
  keep the tracks.
- **Stream.** `stream?id=<id>` with token parameters, through a seekable `QIODevice` that fetches 1 MiB chunks with
  range requests, prefetches two chunks ahead and retries a failed chunk twice. A server that ignores range requests
  sends the whole file, which is read while it downloads; a seek past the downloaded part waits for it.
- **Cover art.** `getCoverArt?id=<coverArt>`, through `resolveUrl()`.
- **Plays.** On `PlayerController::currentTrackChanged`, `scrobble?id=..&submission=false` (now playing). On
  `trackPlayed` (fooyin's "play counts" moment), `scrobble?id=..&submission=true&time=<ms>`. This is always sent.
- **Client-side scrobbling.** Each account has the option "Also scrobble this server's plays with mirage's
  scrobbler". It decides `reportsPlays(track)` for the server's tracks. Until the user sets it, it follows `getUser`'s
  `scrobblingEnabled` for the signed-in user: off when the server scrobbles to Last.fm itself, on otherwise (also when
  the server does not say). Only a choice that differs from that default is stored, in the account's `Scrobble` value
  (`1` or `0`; empty follows the server). `getUser` is asked when an account loads, is saved and syncs; the page shows
  what the server said.
- **Search.** `searchTracks()` calls `search3` with the query, `songCount=50` and no artists or albums. Each song
  becomes a non-library track built like a synced one, so it plays, scrobbles and can be added to playlists. oasis
  returns YouTube Music songs after the library's matches, and these appear in the search; a server that returns only
  library songs adds nothing.
- **Lyrics.** The lyrics plugin has a "Subsonic" source for tracks of Subsonic libraries. It calls OpenSubsonic
  `getLyricsBySongId` if `getOpenSubsonicExtensions` lists `songLyrics` (asked once per server), and `getLyrics` by
  artist and title otherwise. `structuredLyrics` become LRC text: the first synced entry with its `offset`, or else plain
  lines. The request is signed by the provider: `resolveUrl()` maps `<library path>/rest/<method>?<params>` to an
  authenticated API URL. The server holds the track's own lyrics, so the source counts as local: it runs without
  fooyin's automatic search, after lyrics files and embedded tags (which win), before lyrics services, and its lyrics
  are not saved back. With the `romanization` extension and the display option set to romanized or both, it asks
  `enhanced=true` and `kinds=pronunciation` or `kinds=main,pronunciation`, and shows the `pronunciation` entry, or each
  of its lines after the matching `main` line. `cueLine` word timing becomes enhanced LRC word timestamps. Entries are
  picked by `kind`, so a server that ignores `kinds` works too, and a song without a pronunciation entry shows its main
  lyrics. A positive `offset` shows the lyrics sooner, as in LRC.
- **Romanized names.** If `getOpenSubsonicExtensions` lists `romanization` v1 (asked when an account loads, is saved
  and syncs), sync, search and `getPlaylist` send `romanization=both`. The `…Latin` fields of songs become extra tags:
  `TITLELATIN`, `ALBUMLATIN`, `ARTISTLATIN` (from `artists[].nameLatin`, or `artistLatin`), `TRACKARTISTLATIN`
  (the artists that `%trackartist%` lists) and `ALBUMARTISTLATIN` (from `albumArtists[].nameLatin`, or
  `displayAlbumArtistLatin`). They are set only where the server sends them. The core script function
  `$display_name(original,romanized)` shows a name as the Subsonic page's "Show names and lyrics" option says
  (original, romanized, or "original (romanized)"); a missing romanized form shows the original. It lives in core
  (`core/scripting/displayname.h`), so the default views keep their names with the plugin disabled; the plugin owns
  the option and passes it to `Scripting::setNameDisplay()`, and without the plugin the function shows the original. The default library
  tree groupings, playlist presets and columns, and window title use it (`[mirage]` commits), for example
  `$display_name(%title%,%titlelatin%)`. The default filters do not, because they split multi-value artists. Changing the option refreshes the library tracks that have romanized names.
  The option is disabled, with a hint, while no server lists the extension. Server playlist names, `displayComposer`
  and album and artist objects are not used, because the plugin does not show them.
- **Ratings and stars.** On `tracksStatsChanged`: a rating change calls `setRating`, rounded to 1–5 stars, or 0 to
  clear. A love change calls `star` or `unstar`.
- **Playlists.** After each sync, `getPlaylists` and `getPlaylist` for each one. Each server playlist becomes a fooyin
  playlist, matched by a `SubsonicPlaylist` extra property, and is created, renamed, replaced or removed to match.
  Edits in fooyin go back to the server one second after the last change: track changes through `createPlaylist` with
  `playlistId` (which replaces the entries), renames through `updatePlaylist`, and removal through `deletePlaylist`.
  A playlist with edits the server has not saved is marked with a `SubsonicUnsaved` extra property, so the mark
  survives a restart. A sync does not overwrite it, and the push is retried after the next sync. Removals that fail are
  retried on the next sync in the same session. When a sync applies the server's entries, entries the server cannot
  hold (local files, tracks from other servers) keep their place. Songs that are not in the library (for example
  oasis's radio tracks) are kept as they are, so a push sends them back. Changes that only refresh the metadata of
  entries, as after a sync, are not edits. The last write wins. Removing a mirrored playlist asks for confirmation
  (unless turned off), and the dialog says that the server copy is deleted too.
- **Read-only playlists.** A playlist the server marks `readonly` (OpenSubsonic), or one owned by another user, gets a
  `SubsonicReadOnly` extra property and is locked in fooyin. Edits to it are not sent, and removing it only removes the
  fooyin copy, which comes back on the next sync. If the server refuses a push for good (error 50 or 70), the edits
  are dropped and the server's copy is applied again.
- **New playlists.** Every new normal playlist is created on the default server (the Subsonic library with the lowest
  id) with `createPlaylist` and linked through the returned id; for servers older than API 1.14, which return nothing,
  it is found by name. After each sync, playlists that are still unlinked (made before a server was added, or whose
  upload failed) are uploaded. fooyin's empty "Default" playlist is uploaded only once it gets tracks. Temporary playlists and autoplaylists stay local, because Subsonic cannot store
  autoplaylist queries.
- **Accounts.** The settings page lists the servers. Selecting one edits its name, URL, user name or password; an empty
  password keeps the stored one. The library path stays the same, so tracks, statistics and playlists are kept when
  the server moves.
- **Errors.** API errors carry the Subsonic error code. Error messages never include request URLs, because those carry
  the authentication token.
- **Refresh.** The plugin syncs its libraries once tracks have loaded at startup, because server content changes
  without local file changes. When fooyin's own "auto refresh" is on, that refresh covers it instead. "Scan for
  changes" and the library settings page also sync.

## Local media

mirage has no local files, so fooyin's features for local files and local media (CDs) are hidden or turned off. The
code stays, so rebasing on fooyin stays cheap and a feature comes back by removing its id from one list.

- **The gate.** `Mirage::isLocalMediaId()` (`utils/mirage.h`) lists the ids of actions, track menu entries, settings
  pages and widgets for local media. `ActionManager::registerAction()` hides those actions, which also turns off their
  shortcuts; the track context menus and their settings page skip those entries; the settings dialog skips those
  pages; and `WidgetProvider` does not offer those widgets. Hidden this way:
  - File: Add files, Add folders, Load playlist, Save playlist, Save all playlists, Open audio CD (cdda)
  - track menus: Open containing folder, Copy file path, Copy directory path, artwork search, extraction, attaching
    and removal, the Tagging submenu (metadata lookup, Reload tags, the quick tagger and the tag editor's auto-fill),
    Convert, Verify integrity, Verify with AccurateRip, and the Lyrics submenu (edit, search and save lyrics of
    selected tracks)
  - settings pages: Playlist > Saving, Lyrics > Saving, Interface > Artwork > Download, Integrations > System (opening
    files), Playback > Decoding (the decoders stay; server streams always use FFmpeg), Tagging > Quick Tagger
  - widgets: Directory Browser
- **Read-only metadata.** Subsonic cannot edit song metadata. `Track::isMetadataEditable()` is false for tracks
  without a local file (`isDatabaseOnlyMetadata()`), so the tag editor, playlist cell editing and the lyrics editor
  are read-only for server tracks. Ratings and loves still go to the server. The properties dialog is the read-only
  full view: the tag editor's Metadata tab lists only the fields the server sent, the Details tab shows extended
  metadata (all extra tags) and ReplayGain by default and adds disc, composer, performer, comment and rating to its
  default fields, and Apply is hidden for server tracks. The ReplayGain and Artwork editor tabs are not registered.
- **No local writes for server tracks.** The lyrics saver refuses server tracks (lyrics from services are shown but not
  saved), the cover widget and lyrics widget hide their write and extract actions for them, and
  `Utils::File::openDirectory()` ignores paths that are not local folders, which catches "Open containing folder"
  still set as a click action or script command in an older configuration. The status bar and cover widget no longer
  offer that click action, and the `playingfolder` script command is unavailable for server tracks.
- **No local files in.** `GuiApplication::openFiles()` drops local files (command line and second instances), the
  desktop file has no `%F` and no MIME types, and drags from other applications that carry local files are refused.
  Drags that start inside mirage (library tree nodes and tracks into playlists, between playlists and to the queue)
  work as in fooyin, and plugin packages can still be dropped onto the plugins page.
- **Plugins off by default.** File Operations, the ReplayGain scanner and Run Services are in the default
  `Plugins/Disabled` list and can be turned on in Settings > Plugins. The tag editor, quick tagger and cdda plugins
  stay loaded for the planned CD upload (see "Backlog"); their actions are hidden by the gate.
- **Folder libraries from older builds** stay in the database. `Application` calls
  `LibraryManager::setProvidersOnly(true)`, which hides them and their tracks; they are not scanned or monitored.
- **Not hidden:** Add stream URL, waveform data (Utilities; it decodes the stream into mirage's own cache), layout,
  theme and preset import and export, the plugin installer, and the now-playing text output. Local tracks already in
  playlists from older builds stay and play.

## Branches

- `library-providers`, on `master`: only the generic API commits, each tagged `[core]`, `[gui]` or `[scrobbler]` as
  in upstream, kept as a small series that rebases onto upstream `master` (see "Keeping the fork rebased").
- `mirage`, on `library-providers`: this document, the Subsonic plugin (`[subsonic]` commits) and two small
  `[mirage]` commits: the application name and profile, install names (binary `mirage`, `lib/mirage`,
  `share/mirage`, desktop file, metainfo and icons as `io.github.zakius.mirage`, so it installs next to fooyin), and
  Subsonic-only libraries (`LibraryManager` refuses folder
  paths; the library settings page hides folder settings and sends "Add" to the Subsonic page), and the `[mirage]`
  commits that hide local media (see "Local media"). Rebase it when the API branch changes.

## Order of work

1. This plan.
2. `library-providers`:
   1. `LibraryProvider` and `LibraryProviderRegistry`, exposed in `CorePluginContext`.
   2. Provider sync in the scan pipeline (`syncLibrary`), with a unit test that a second identical sync writes nothing
      and that removed tracks are disabled.
   3. Skip provider libraries in the monitor and in `scanTracks`.
   4. Playback through `handlesScheme()` and `createStreamSource()` in `AudioLoader` and `RemoteIoService`, with a test.
   5. Provider artwork in `CoverRepository` and `RemoteIoService::download()`.
   6. The scrobbler skips tracks whose provider reports plays.
   7. Build and run the existing test suite.
3. `mirage`:
   1. Subsonic client and account settings.
   2. Sync to tracks, and stream and cover resolution.
   3. Scrobbles, rating and star write-back.
   4. Playlist sync.
   5. End-to-end test against a real server (oasis with a generated library): library tree, search, playback, seeking,
      cover art, and play counts in `getSong`.

## Keeping the fork rebased

Upstream does not accept code written with generative AI, so mirage stays a fork. The patches are kept small so that
they rebase easily:

- `library-providers` is a short series on top of upstream `master` (about 420 changed lines in `src` and `include`,
  plus tests). It adds one header and one source file and touches existing files only at narrow points.
- `mirage` adds the plugin in its own directory. Outside it, it changes `src/plugins/CMakeLists.txt` and a few lines
  for the application name, the library settings page and `LibraryManager::addLibrary()`.

To update:

```
git fetch upstream
git rebase upstream/master library-providers
git rebase library-providers mirage
cmake --build --preset debug && ctest --preset debug
```

## Status (2026-11-01)

Steps 1 to 3 are done. Verified against oasis with a generated 30-track library, in an isolated fooyin profile that
plays to a PulseAudio null sink. Fault cases used a proxy in front of oasis.

- Adding a server on the settings page syncs all tracks into the library tree, filters and library search.
- Editing a server's URL and name keeps the library; an empty password field keeps the password.
- Playback, and seeking forward and backward through range requests.
- A server that ignores range requests: playback starts while the file downloads, and seeking still works.
- Failed stream requests (every third one returning HTTP 500) are retried and playback continues.
- Cover art, both embedded and from folder images.
- Plays reach the server: `getSong` play counts increase once fooyin's played threshold passes. A ListenBrainz service
  receives plays of local tracks but nothing for server tracks.
- Ratings and stars set in fooyin reach the server, and unstarring works.
- Server changes (one track added, one removed, one rated) arrive on the next sync as exactly those changes. A sync of
  unchanged data writes nothing.
- A server without empty `search3` support is synced through album listing, with the same result.
- A failed sync (wrong password) keeps the tracks.
- Server playlists appear in fooyin. Adding, removing and renaming in fooyin reach the server, and removing the
  playlist in fooyin deletes it on the server. Playlists deleted on the server disappear from fooyin.
- Library and playlist entries survive a restart.
- mirage runs with its own profile (`~/.config/mirage`, `~/.local/share/mirage`, `~/.local/state/mirage`) and MPRIS name
  `org.mpris.MediaPlayer2.mirage`.
- Folder libraries cannot be added; "Add" on the library page opens the Subsonic page.
- A playlist that existed before the server was added, a new empty playlist, a playlist created from a library
  selection, and tracks added later all reach the server. A restart does not upload them again.
- A local file opened into a server playlist stays there after the server adds songs to that playlist; the new songs
  are merged around it.
- With the server down: a removed entry and a removed playlist reach the server after "Scan for changes" once it is
  back. An entry removed while the server is down, followed by a restart, reaches the server on the next start.
- `cmake --install` puts nothing under a `fooyin` path.

Verified on 2026-11-01 against oasis master with the `romanization` extension, with a generated library of Japanese,
Korean, Chinese and Latin tags and `.lrc` files:

- The extension is detected and enables the display option. The library tree, playlist headers and columns and the
  window title show original, romanized or "original (romanized)" names as set, and Latin names stay as they are.
- Romanized lyrics: "Romanized" shows the `pronunciation` lines, "Original and romanized" each main line followed by
  its romanization, both synced. oasis sends no `cueLine` yet, so word timing is covered by unit tests only.
- The scrobbling hint and default follow `getUser`: "doesn't scrobble" and on for a user without Last.fm, "scrobbles"
  and off for one with it.
- With the Subsonic plugin disabled, the default views show the original names.

Verified on 2026-11-01 against oasis master with a generated 5-track library (FLAC and MP3), in an isolated profile on
Xvfb that plays to a PulseAudio null sink:

- The File menu offers only Add stream URL, New playlist, New autoplaylist and Quit. The Edit, View, Layout, Playback
  and Library menus have no local-file actions. Settings has no Decoding, Playlist Saving, Lyrics Saving,
  Integrations > System or Quick Tagger page; the ReplayGain scanner is off.
- The track context menus of the library tree, playlists and status bar, the cover widget menu, the playlist tab menu
  and the lyrics widget menu have no local-file actions. Artwork offers only View full size.
- Properties of a server track: the Metadata tab is read-only and lists only the fields the server sent, the Details
  tab shows them all, and there is no Apply.
- Dragging an artist node from the library tree into a playlist adds all its tracks. Dragging tracks onto another
  playlist's tab and into the playback queue works. A local file dragged in from another application is refused; an
  http URL dragged the same way is accepted.
- A local file passed to a second instance on the command line opens nothing.
- Nothing is written next to the music or under a `subsonic:` path.

Limits:

- New playlists go to the default server. Moving a playlist to another server is not supported.
- Autoplaylists stay local (not tested end to end).
- Tracks that are not on the playlist's server (local files, other servers) stay in the fooyin playlist but are not
  sent.
- A playlist removal that fails is forgotten on restart, and the playlist comes back on the next sync.
- Playlist conflicts resolve by last write.
- Lyrics already shown for the playing track keep their layers when the display option changes, until they are
  searched again.
- Changing the name display option rewrites the database rows of the affected tracks (with unchanged values), because
  that is how views learn that their names changed.
- Playlists are read again only after a library sync (at start, or "Scan for changes"), so a playlist the server
  grows (oasis radio) lags until then.

## Backlog

**CD ripping and album upload to the server (needs the server's `upload` extension).**

- Only for servers listing an `upload` OpenSubsonic extension; hidden on every other server.
- Flow: read the disc's TOC and MusicBrainz disc id (the cdda plugin's `cddatoc.h` already computes it) →
  `lookupDisc` (the server returns MusicBrainz release candidates) → `createUpload` with the chosen release or user
  tags → `uploadTrack` per track as FLAC encoded locally (via the `formPost` extension), resumable → `commitUpload`
  (the server tags, names, adds cover art, scans; nothing appears half-done).
- The same flow uploads an existing file or album without a CD.
- Later: AccurateRip verification.
- Desktop only; caravan won't have it.

The cdda plugin, the tag editor and the quick tagger stay in the build for this; the local media gate hides their
actions until then.

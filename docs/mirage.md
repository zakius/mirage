# mirage: Subsonic libraries in fooyin

mirage is a fooyin fork that shows a remote Subsonic library as part of fooyin's own library. Server tracks appear in
the library tree, filters, search, columns and autoplaylists exactly like local files. There is no separate browser
panel and no "download to library" step.

The plugin talks to any Subsonic or OpenSubsonic server through the standard API and treats all servers the same.
Server-specific behaviour (for example oasis merging a local library with a streaming service) stays on the server
and is invisible to the client.

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
    //! True if the provider reports plays to its own service, so client-side scrobblers skip its tracks.
    [[nodiscard]] virtual bool reportsPlays() const;
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
`refresh()`, `rescan()`, "auto refresh on startup" and adding a library all go through this path.

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

- Provider tracks are virtual, so `Track::exists()` is already true and "remove unavailable tracks" keeps them.
- A rescan of a provider library goes to the provider. A track disappears only when the server stops listing it.
- `LibraryMonitor` skips provider libraries, because they have no directory to watch.
- "Reload tags" (`scanTracks`) skips provider tracks, because there is no file to read.

### Scrobbling

The scrobbler plugin skips tracks whose provider returns `reportsPlays()`. The Subsonic server receives the plays and
can forward them itself, so Last.fm does not count them twice.

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

  If a server does not accept an empty `search3` query, the plugin falls back to walking `getAlbumList2`
  (`alphabeticalByName`) and `getAlbum`.
- **Stream.** `stream?id=<id>` with token parameters, through a seekable range-request `QIODevice`.
- **Cover art.** `getCoverArt?id=<coverArt>`, through `resolveUrl()`.
- **Plays.** On `PlayerController::currentTrackChanged`, `scrobble?id=..&submission=false` (now playing). On
  `trackPlayed` (fooyin's "play counts" moment), `scrobble?id=..&submission=true&time=<ms>`. `reportsPlays()` returns
  `true`.
- **Ratings and stars.** On `tracksStatsChanged`: a rating change calls `setRating`, rounded to 1–5 stars, or 0 to
  clear. A love change calls `star` or `unstar`.
- **Playlists.** After each sync, `getPlaylists` and `getPlaylist` for each one. Each server playlist becomes a fooyin
  playlist, matched by a `SubsonicPlaylist` extra property, and is created or replaced. The sync is one-way, from the
  server to fooyin.
- **Refresh.** Startup auto refresh and the library settings page refresh button.

## Branches

- `library-providers`, on `master`: only the generic API commits, each tagged `[core]`, `[gui]` or `[scrobbler]` as
  in upstream. This branch is the candidate for an upstream pull request. Note that upstream does not accept code
  written with generative AI (`CONTRIBUTING.md`), so offering it upstream needs the user's decision.
- `mirage`, on `library-providers`: this document and the Subsonic plugin (`[subsonic]` commits). Rebase it when the
  API branch changes.

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

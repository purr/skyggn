# changelog

what changed in each release of skyggn. a release's section here is also its text on the
releases page.

## [0.1.2]

- **make thumbnails now**, on the app's general page: paste or pick a folder, with or without its
  subfolders, and skyggn has windows make the thumbnails its files do not have yet, three at a time,
  with a progress bar that can stop. opened in file explorer afterwards, the folder shows its
  thumbnails at once. online-only cloud files are left alone, so nothing is downloaded. `skyggnctl prepare
  <folder> [--recursive]` does the same from a prompt, and `skyggnctl refresh` now makes three at a
  time too.

## [0.1.1]

- thumbnails come several times faster. windows asks for them at 1280 px to fill its cache, and
  the frosted glass behind the badge took half a second at that size; it now takes a few
  milliseconds, with the very same look. making a full-hd video's thumbnail took about 545 ms at that
  size, and now takes about 75 ms.
- gentle mode keeps normal priority and only makes each thumbnail on one thread. with a lowered
  priority, thumbnails waited seconds behind any busy program, such as a compile.
- installing an update no longer stops with "unable to close all applications" (or, silently,
  gives up) while windows' thumbnail helper still has skyggn loaded: the loaded files are moved
  aside and removed when windows next starts.
- file icons in explorer (photos, videos and other types an app opens) no longer carry skyggn's
  badge. windows draws those icons from the apps' own pictures, named like
  `name.targetsize-256.png`, through the thumbnail handlers, and skyggn put its badge on them too.
  click **refresh thumbnails** in the app once: it now clears windows' icon cache as well, and
  always restarts file explorer, so the old icons go.

## [0.1.0]

the first release of skyggn, an open-source alternative to icaros.

- thumbnails in file explorer, file dialogs and every app that asks windows for them, for 179 file
  types: 46 video, 32 audio (cover art), 47 image, 38 camera raw, 5 comic book and e-book, and 11
  document formats.
- videos show a frame from a chosen point (20 % in by default), skipping black, white and empty
  frames, or the cover a video file carries. hdr video (hdr10, hlg) is tone-mapped so it does not look
  washed out, and phone videos are turned upright.
- songs show their cover art, from mp3, flac, ogg, opus, m4a, mka and more.
- photos: jpeg, png, heic (including the tiled ones phones make), avif, jpeg xl, webp, psd, exr,
  svg, dds and others, with transparency kept and phone photos turned upright.
- adobe design files show the preview they carry: illustrator (ai, ait) its picture of the
  artwork, eps its tiff preview, indesign (indd, indt) its first page.
- camera raws from canon, nikon, sony, fujifilm, panasonic, olympus, pentax, leica, sigma,
  hasselblad, phase one and others show the preview the camera embedded; a file with no preview,
  or only a tiny one, is developed from its sensor data instead.
- comics (cbz, cbr, cb7, cbt) show their first page, in the order explorer sorts names; e-books
  (epub) show the cover their package names; pdfs their first page, drawn by windows' own pdf
  renderer; libreoffice and openoffice documents (odt, ods, odp, odg and their templates) the
  preview saved with them.
- explorer's details for the formats windows describes poorly or not at all: length, frame size,
  frame rate, bit rates, codecs, channels, sample rate, title, artist, album and other tags, in
  the details pane, the properties dialog and the details view's columns. four columns windows
  does not have: video tracks, audio tracks and subtitles with their languages, and chapters. flv,
  ape, wavpack, tak, tta, musepack, rmvb, divx, mxf, aiff, caf, dsd and others. windows keeps mkv
  and webm details for its own handler; an option on the general page hands them to skyggn, and
  puts windows' entries back when turned off.
  read in a separate process, so a damaged file cannot crash explorer. for installs for every
  user.
- a badge on each thumbnail says what kind of file it is: frosted glass, coloured by kind, or
  with the file type, on any corner, in any size from 16 to 32 % of the thumbnail, or none.
- files without a picture (a song without cover art, a damaged video) get a tile with a big
  symbol and their file type, in colours made from their contents (copies match whatever their
  names, a whole album matches), or windows' usual icon.
- damaged files say so, only when their own structure proves it: an empty file, one cut off (a
  half download, a recording that stopped, an mp4 without its index, an archive without its table
  of contents) or a corrupted header. a small warning mark shows on the badge, or on the tile of a
  file without a picture, which also names it under the file type. `skyggnctl check <file>` tells
  the same.
- no background process: windows runs skyggn only while it makes a thumbnail or reads a file's
  details, in a separate process, so a broken file cannot crash explorer. gentle mode and a time limit per file keep it
  light.
- the settings app: turn skyggn on or off, choose file types, change the look with a live
  preview, refresh thumbnails, and remove broken entries left by removed thumbnail programs.
- the settings app and the installer in english and german, following windows' language; the app's
  language can also be chosen on its general page.
- every file type gets its previous thumbnail handler back when it is turned off or skyggn is
  uninstalled.
- `skyggnctl`, a command line tool for the same, and scripted installs.

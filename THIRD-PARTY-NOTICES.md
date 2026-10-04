# third-party notices

## ffmpeg

skyggn ships ffmpeg's shared libraries (`avformat`, `avcodec`, `avutil`, `swscale`, `swresample`)
unmodified, next to `skyggn-engine.dll`, and loads them at runtime.

- version: 9.0.2 (release/9.0 branch, commit
  [2a571b6068](https://github.com/FFmpeg/FFmpeg/commit/2a571b606854520cf89804d8030c8b328e621689))
- build: [btbn/ffmpeg-builds](https://github.com/BtbN/FFmpeg-Builds), `win64-lgpl-shared-9.0`, from
  the `autobuild-2026-09-30-13-08` release. the build scripts there reproduce it.
- license: gnu lesser general public license, version 3 or later (the build is configured with
  `--enable-version3`). the full text ships as `licenses/ffmpeg-license.txt`.
- the build includes further libraries (among them dav1d, libvpx and libaom) under their own
  licenses; `ffmpeg -buildconf` from the same build lists them.

## libraw

compiled into `skyggn-engine.dll`, unmodified, to read camera raw files: the preview the camera
embedded, or the sensor data of a file without a usable one.

- version: 0.22.2, https://github.com/LibRaw/LibRaw
- license: gnu lesser general public license, version 2.1, or the common development and
  distribution license 1.0, at the user's choice; skyggn uses it under the lgpl-2.1. the license
  ships as `licenses/libraw-license.txt`, libraw's copyright notice (which credits dave coffin's dcraw and
  the bsd-licensed dcb and fbdd code) as `licenses/libraw-copyright.txt`. skyggn's complete source is
  public, so libraw can be replaced and the engine relinked.

## libarchive, zlib and xz

compiled into `skyggn-engine.dll`, unmodified, to open comic book archives (zip, rar, 7z, tar) and
e-books.

- libarchive 3.8.9, https://github.com/libarchive/libarchive: the 2-clause bsd license, with a
  few files under other permissive terms (a 3-clause university of california license, the public
  domain, and cc0 1.0, openssl or apache 2.0 for its blake2 code).
  `licenses/libarchive-license.txt` reproduces all of them.
- zlib 1.3.2, https://zlib.net: the zlib license, `(C) 1995-2026 Jean-loup Gailly and Mark Adler`,
  as `licenses/zlib-license.txt`.
- xz 5.8.4, https://tukaani.org/xz/: its liblzma is under the bsd zero clause license, as
  `licenses/xz-license.txt`.

## windows app sdk

the settings app ships the windows app sdk 2.5.1 runtime files next to `skyggn.exe`
(self-contained), under microsoft's license terms for the windows app sdk, which allow
redistributing the files the package places with an application. the terms ship as
`licenses/windows-app-sdk-license.txt`.

## .net runtime and windows community toolkit

the .net runtime is compiled into `skyggn.exe` (native aot); the windows community toolkit's
settings controls are part of it too. both are under the mit license:
https://github.com/dotnet/runtime, https://github.com/CommunityToolkit/Windows

```
The MIT License (MIT)

Copyright (c) .NET Foundation and Contributors

All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## windows implementation library (wil)

compiled into `skyggn-engine.dll`. https://github.com/microsoft/wil

```
MIT License

Copyright (c) Microsoft Corporation. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE
```

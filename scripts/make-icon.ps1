# renders assets/skyggn.svg into app/Assets/skyggn.ico, at the sizes windows uses for an app icon.
# uses the ffmpeg that cmake downloads (configure the build once first).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'build\_deps\ffmpeg-src\bin\ffmpeg.exe'
if (-not (Test-Path $ffmpeg)) {
    throw "make-icon: $ffmpeg not found; run 'cmake --preset x64' first"
}
$sizes = 256, 64, 48, 40, 32, 24, 20, 16
$split = "[0:v]split=$($sizes.Count)" + (($sizes | ForEach-Object { "[s$_]" }) -join '')
$scales = $sizes | ForEach-Object { "[s$_]scale=${_}:${_}:flags=lanczos[o$_]" }
$maps = $sizes | ForEach-Object { '-map', "[o$_]" }
& $ffmpeg -hide_banner -loglevel error -y -width 1024 -height 1024 -i (Join-Path $root 'assets\skyggn.svg') `
    -filter_complex ((@($split) + $scales) -join ';') @maps -c:v png -f ico (Join-Path $root 'app\Assets\skyggn.ico')
if ($LASTEXITCODE) {
    throw "make-icon: ffmpeg failed with exit code $LASTEXITCODE"
}

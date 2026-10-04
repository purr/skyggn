# renders the settings app's sample files, which its thumbnails page previews before a file is
# chosen: a short video of assets/sample-picture.svg, and a song without cover art. uses the ffmpeg
# that cmake downloads (configure the build once first).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'build\_deps\ffmpeg-src\bin\ffmpeg.exe'
if (-not (Test-Path $ffmpeg)) {
    throw "make-samples: $ffmpeg not found; run 'cmake --preset x64' first"
}
$samples = Join-Path $root 'app\Assets\samples'
New-Item -ItemType Directory -Force $samples | Out-Null

function Make([string]$name, [string[]]$arguments) {
    & $ffmpeg -hide_banner -loglevel error -y @arguments (Join-Path $samples $name)
    if ($LASTEXITCODE) { throw "make-samples: ffmpeg could not make $name" }
}

# two seconds of the picture, as a camera or phone would save a clip
Make 'video.mkv' @('-loop', '1', '-framerate', '1', '-width', '1280', '-height', '720',
    '-i', (Join-Path $root 'assets\sample-picture.svg'), '-t', '2', '-vf', 'format=yuv420p', '-c:v', 'libopenh264')
# half a second of silence, tagged like an album track but without a cover
Make 'song.mp3' @('-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo', '-t', '0.5', '-c:a', 'libmp3lame',
    '-metadata', 'title=Evening', '-metadata', 'album=Sample Album', '-metadata', 'artist=skyggn')
Get-ChildItem $samples | ForEach-Object { '{0} {1:n0} KB' -f $_.Name, ($_.Length / 1KB) }

# renders assets/showcase.png, the row of thumbnails in the readme. the samples are made up by
# ffmpeg's pattern generators, so the picture holds no one's photos; the thumbnails are the
# engine's real output. needs the release build (cmake --build --preset release). your skyggn
# settings are put back afterwards.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'build\_deps\ffmpeg-src\bin\ffmpeg.exe'
$ctl = Join-Path $root 'build\bin\Release\skyggnctl.exe'
foreach ($tool in $ffmpeg, $ctl) {
    if (-not (Test-Path $tool)) { throw "make-showcase: $tool not found; build the release first" }
}
$work = Join-Path $root 'build\showcase'
New-Item -ItemType Directory -Force $work | Out-Null

function Make([string]$name, [string[]]$arguments) {
    & $ffmpeg -hide_banner -loglevel error -y @arguments (Join-Path $work $name)
    if ($LASTEXITCODE) { throw "make-showcase: ffmpeg could not make $name" }
}

$tone = @('-f', 'lavfi', '-i', 'sine=frequency=440:duration=2')
Make 'cover.png' @('-f', 'lavfi', '-i', 'gradients=s=800x800:c0=0xff5f6d:c1=0xffc371:c2=0x6a3093:n=3:type=radial:seed=7', '-frames:v', '1')
Make 'film.mkv' @('-f', 'lavfi', '-i', 'gradients=s=1280x720:c0=0x4568dc:c1=0xb06ab3:c2=0xff9a8b:n=3:type=circular:seed=5:d=2', '-c:v', 'libopenh264')
Make 'song.mp3' ($tone + @('-i', (Join-Path $work 'cover.png'), '-map', '0', '-map', '1', '-c:a', 'libmp3lame', '-c:v', 'mjpeg', '-disposition:v', 'attached_pic'))
# a sunset: sky fading to the horizon, a sun, two rows of hills
$hills = 'gt(Y,H*(0.72+0.05*sin(2*PI*X/W*0.9+2)))'
$far = 'gt(Y,H*(0.60+0.04*sin(2*PI*X/W*1.3+0.5)))'
$sun = 'lt(hypot(X-0.68*W,Y-0.52*H),0.07*W)'
$sky = 'st(1,min(Y/H/0.65,1))'
$channel = { param($near, $back, $disc, $top, $horizon) "$sky;if($hills,$near,if($far,$back,if($sun,$disc,$top+($horizon-$top)*ld(1))))" }
$geq = "r='$(& $channel 45 106 255 30 247)':g='$(& $channel 30 76 226 60 178)':b='$(& $channel 74 147 154 114 103)'"
Make 'sunset.jpg' @('-f', 'lavfi', '-i', "color=s=1200x800,format=rgb24,geq=$geq", '-frames:v', '1', '-q:v', '2')
Make 'phone clip.mp4' @('-f', 'lavfi', '-i', 'gradients=s=720x1280:c0=0x11998e:c1=0x38ef7d:c2=0x1d2671:n=3:type=linear:seed=11:d=2', '-c:v', 'libopenh264')
Make 'no cover.flac' ($tone + @('-metadata', 'album=Evening Tapes', '-c:a', 'flac'))

# each sample with its badge style: 1 frosted, 2 coloured by kind, 3 with the file type
$shots = @(
    @{ File = 'film.mkv'; Style = 1 }
    @{ File = 'song.mp3'; Style = 2 }
    @{ File = 'sunset.jpg'; Style = 3 }
    @{ File = 'phone clip.mp4'; Style = 1 }
    @{ File = 'no cover.flac'; Style = 1 }
)
$saved = @{}
foreach ($line in (& $ctl settings | Select-Object -Skip 1)) {
    if ($line -match '^(\w+)\s+(\d+)\s') { $saved[$Matches[1]] = $Matches[2] }
}
try {
    foreach ($name in 'BadgeCorner', 'BadgeSize', 'Placeholder') {
        & $ctl set $name ((& $ctl settings | Select-String "^$name\s+\d+\s+(\d+)").Matches[0].Groups[1].Value) | Out-Null
    }
    $index = 0
    foreach ($shot in $shots) {
        & $ctl set BadgeStyle $shot.Style | Out-Null
        & $ctl thumb (Join-Path $work $shot.File) (Join-Path $work "shot$index.png") --size 256
        if ($LASTEXITCODE) { throw "make-showcase: no thumbnail for $($shot.File)" }
        $index++
    }
} finally {
    foreach ($name in $saved.Keys) { & $ctl set $name $saved[$name] | Out-Null }
}

# one row, each thumbnail centred in a 288 px cell, on a transparent background
$inputs = 0..($shots.Count - 1) | ForEach-Object { '-i', (Join-Path $work "shot$_.png") }
$cells = (0..($shots.Count - 1) | ForEach-Object { "[$_]format=rgba,pad=288:288:(ow-iw)/2:(oh-ih)/2:color=0x00000000[c$_]" }) -join ';'
$row = (0..($shots.Count - 1) | ForEach-Object { "[c$_]" }) -join ''
& $ffmpeg -hide_banner -loglevel error -y @inputs -filter_complex "$cells;${row}hstack=inputs=$($shots.Count)" `
    -frames:v 1 (Join-Path $root 'assets\showcase.png')
if ($LASTEXITCODE) { throw "make-showcase: ffmpeg could not put the row together" }

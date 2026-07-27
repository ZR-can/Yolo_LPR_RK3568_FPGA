[CmdletBinding()]
param(
    [string]$VideoDirectory = "",
    [string]$ModelDirectory = "",
    [string]$FFmpegPath = "",
    [ValidateRange(1, 1000)]
    [int]$FramesPerVideo = 25,
    [ValidateRange(32, 4096)]
    [int]$ImageSize = 640
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-FFmpegExecutable {
    param([string]$ExplicitPath)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($ExplicitPath) {
        $candidates.Add($ExplicitPath)
    }
    if ($env:FFMPEG_PATH) {
        $candidates.Add($env:FFMPEG_PATH)
    }

    $command = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($command -and $command.Source) {
        $candidates.Add($command.Source)
    }

    foreach ($entry in ($env:Path -split ';')) {
        $trimmed = $entry.Trim().Trim('"')
        if (-not $trimmed) {
            continue
        }
        if ([System.IO.Path]::GetFileName($trimmed) -ieq "ffmpeg.exe") {
            $candidates.Add($trimmed)
        } else {
            $candidates.Add((Join-Path $trimmed "ffmpeg.exe"))
        }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "ffmpeg.exe was not found. Pass -FFmpegPath or set FFMPEG_PATH."
}

function Resolve-ChildPath {
    param(
        [string]$Parent,
        [string]$ChildName
    )

    $parentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    $childFull = [System.IO.Path]::GetFullPath((Join-Path $parentFull $ChildName))
    $prefix = $parentFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $childFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside model directory: $childFull"
    }
    return $childFull
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDirectory "..\..\.."))
if (-not $VideoDirectory) {
    $VideoDirectory = Join-Path $workspaceRoot "ITS\videoready"
}
if (-not $ModelDirectory) {
    $ModelDirectory = Join-Path $scriptDirectory "..\model"
}

$videoDirectoryFull = [System.IO.Path]::GetFullPath($VideoDirectory)
$modelDirectoryFull = [System.IO.Path]::GetFullPath($ModelDirectory)
if (-not (Test-Path -LiteralPath $videoDirectoryFull -PathType Container)) {
    throw "Video directory does not exist: $videoDirectoryFull"
}
if (-not (Test-Path -LiteralPath $modelDirectoryFull -PathType Container)) {
    throw "Model directory does not exist: $modelDirectoryFull"
}

$ffmpeg = Resolve-FFmpegExecutable -ExplicitPath $FFmpegPath
$ffprobe = Join-Path (Split-Path -Parent $ffmpeg) "ffprobe.exe"
if (-not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
    throw "ffprobe.exe was not found next to ffmpeg.exe: $ffprobe"
}

$outputDirectory = Resolve-ChildPath -Parent $modelDirectoryFull -ChildName "traffic_quant_dataset"
$datasetFile = Resolve-ChildPath -Parent $modelDirectoryFull -ChildName "traffic_dataset.txt"

$videoExtensions = @(".mp4", ".mov", ".mkv", ".avi", ".m4v")
$videos = Get-ChildItem -LiteralPath $videoDirectoryFull -File |
    Where-Object { $videoExtensions -contains $_.Extension.ToLowerInvariant() } |
    Sort-Object Name
if ($videos.Count -eq 0) {
    throw "No supported videos found in: $videoDirectoryFull"
}

if (Test-Path -LiteralPath $outputDirectory) {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null

$invariant = [System.Globalization.CultureInfo]::InvariantCulture
$expectedTotal = 0
foreach ($video in $videos) {
    $durationOutput = @(& $ffprobe -v error -show_entries format=duration `
        -of "default=noprint_wrappers=1:nokey=1" $video.FullName)
    $probeExitCode = $LASTEXITCODE
    if ($probeExitCode -ne 0) {
        throw "ffprobe failed: $($video.FullName)"
    }
    $durationText = $durationOutput | Select-Object -First 1

    $duration = 0.0
    if (-not [double]::TryParse($durationText, [System.Globalization.NumberStyles]::Float,
            $invariant, [ref]$duration) -or $duration -le 0.0) {
        throw "Invalid video duration for $($video.Name): $durationText"
    }

    $sampleRate = $FramesPerVideo / $duration
    $sampleRateText = $sampleRate.ToString("0.############", $invariant)
    $outputPattern = Join-Path $outputDirectory ($video.BaseName + "_%04d.jpg")
    $filter = "fps=$sampleRateText,scale=${ImageSize}:${ImageSize}:force_original_aspect_ratio=decrease," +
        "pad=${ImageSize}:${ImageSize}:(ow-iw)/2:(oh-ih)/2:color=0x000000"

    & $ffmpeg -nostdin -hide_banner -loglevel error -y -i $video.FullName `
        -vf $filter -frames:v $FramesPerVideo -q:v 2 $outputPattern
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg failed: $($video.FullName)"
    }

    $generated = @(Get-ChildItem -LiteralPath $outputDirectory -File `
        -Filter ($video.BaseName + "_*.jpg"))
    if ($generated.Count -ne $FramesPerVideo) {
        throw "Expected $FramesPerVideo frames from $($video.Name), generated $($generated.Count)."
    }
    $expectedTotal += $generated.Count
    Write-Host ("{0}: duration={1:F3}s frames={2}" -f $video.Name, $duration, $generated.Count)
}

$images = @(Get-ChildItem -LiteralPath $outputDirectory -File -Filter "*.jpg" | Sort-Object Name)
if ($images.Count -ne $expectedTotal) {
    throw "Dataset count mismatch: expected $expectedTotal, found $($images.Count)."
}

$datasetLines = foreach ($image in $images) {
    "./traffic_quant_dataset/$($image.Name)"
}
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines($datasetFile, $datasetLines, $utf8NoBom)

$totalBytes = ($images | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host "Traffic quantization dataset ready"
Write-Host "videos=$($videos.Count)"
Write-Host "images=$($images.Count)"
Write-Host "image_size=${ImageSize}x${ImageSize}"
Write-Host ("image_bytes={0}" -f $totalBytes)
Write-Host "dataset=$datasetFile"
Write-Host "images_dir=$outputDirectory"

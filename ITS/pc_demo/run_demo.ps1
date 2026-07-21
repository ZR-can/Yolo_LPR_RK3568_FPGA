param(
    [Parameter(Mandatory = $false)]
    [string]$Image = "else\Yolo_LPR_RK3568_FPGA\2_Model_Conversion_PC_Simulation\yolov8\model\bus_coco.jpg",

    [Parameter(Mandatory = $false)]
    [string]$SaveDir = ".\outputs\demo",

    [Parameter(Mandatory = $false)]
    [string]$Python = "C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..\..")

if (-not (Test-Path -LiteralPath $Python)) {
    $Python = "python"
}

$imagePath = $Image
if (-not [System.IO.Path]::IsPathRooted($imagePath)) {
    $imagePath = Join-Path $repoRoot $imagePath
}

$savePath = $SaveDir
if (-not [System.IO.Path]::IsPathRooted($savePath)) {
    $savePath = Join-Path $scriptDir $savePath
}

& $Python (Join-Path $scriptDir "detect_traffic.py") `
    --img $imagePath `
    --save-dir $savePath

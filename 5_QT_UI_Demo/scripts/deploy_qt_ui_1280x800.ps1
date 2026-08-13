[CmdletBinding()]
param(
    [string]$AdbPath = 'D:\adb\bin\adb.exe',
    [string]$PackageDir = '',
    [string]$RemoteRoot = '/userdata/yolov8_lpr_pcie_qt_ui_1280x800'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Join-Path $projectRoot 'install\rk356x_linux_aarch64\rknn_yolov8_ppocr_qt_ui_demo\yolov8_ppocr_pcie_qt_ui_1280x800'
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB executable not found: $AdbPath"
}
if (-not (Test-Path -LiteralPath $PackageDir -PathType Container)) {
    throw "Cross-compiled package directory not found: $PackageDir"
}

$requiredFiles = @(
    'yolov8_ppocr_pcie_qt_ui_1280x800',
    'fpga_bar0_ctrl_test',
    'fpga_preproc_ctrl.sh',
    'pango_pci_driver.ko'
)
foreach ($fileName in $requiredFiles) {
    $filePath = Join-Path $PackageDir $fileName
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        throw "Required deployment file not found: $filePath"
    }
}

function Invoke-Adb {
    param([string[]]$Arguments)

    & $AdbPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ADB command failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

$deviceLines = @(& $AdbPath devices)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to query ADB devices.'
}
$onlineDevices = @($deviceLines | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice\s*$' })
if ($onlineDevices.Count -eq 0) {
    throw 'No online ADB device found.'
}

Write-Host "Deploying $PackageDir"
Write-Host "Remote root: $RemoteRoot"

Invoke-Adb @('shell', "rm -rf $RemoteRoot")
Invoke-Adb @('shell', "mkdir -p $RemoteRoot")
Invoke-Adb @('push', $PackageDir, "$RemoteRoot/")

$remotePackage = "$RemoteRoot/yolov8_ppocr_pcie_qt_ui_1280x800"
$remoteLib = "$RemoteRoot/lib"
Invoke-Adb @('push', (Join-Path (Split-Path -Parent $PackageDir) 'lib'), "$RemoteRoot/")
Invoke-Adb @('shell', "chmod 755 $remotePackage/yolov8_ppocr_pcie_qt_ui_1280x800 $remotePackage/fpga_bar0_ctrl_test $remotePackage/fpga_preproc_ctrl.sh")

Invoke-Adb @('shell', "ls -l $remotePackage/yolov8_ppocr_pcie_qt_ui_1280x800 $remotePackage/fpga_bar0_ctrl_test $remotePackage/fpga_preproc_ctrl.sh $remotePackage/pango_pci_driver.ko")
Write-Host 'Deployment completed.'

Param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,

    [string]$RunDir = (Join-Path ([System.IO.Path]::GetTempPath()) ("fpga_pcie_dma_ep_v2_axis_mwr_{0}" -f ([System.Guid]::NewGuid().ToString('N'))))
)

$ErrorActionPreference = 'Stop'

function Convert-ToForwardPath {
    Param([string]$Path)
    return $Path.Replace('\', '/')
}

if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
    throw "ProjectRoot does not exist: $ProjectRoot"
}

foreach ($tool in @('vlib', 'vlog', 'vsim')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "ModelSim tool was not found in PATH: $tool"
    }
}

$rtl = Join-Path $ProjectRoot 'hdl/video/pcie_v2_axis_mwr.v'
$tb = Join-Path $ProjectRoot 'sim/pcie_v2_axis_mwr_tb.v'

if (-not (Test-Path -LiteralPath $rtl -PathType Leaf)) {
    throw "Missing RTL: $rtl"
}

if (-not (Test-Path -LiteralPath $tb -PathType Leaf)) {
    throw "Missing testbench: $tb"
}

New-Item -ItemType Directory -Path $RunDir -Force | Out-Null

Write-Host "Simulation run directory: $RunDir"
Push-Location $RunDir
try {
    & vlib work | Tee-Object -FilePath (Join-Path $RunDir 'vlib.log')
    if ($LASTEXITCODE -ne 0) {
        throw "vlib failed with exit code $LASTEXITCODE"
    }

    $compileLog = Join-Path $RunDir 'vlog.log'
    & vlog -sv -work work (Convert-ToForwardPath (Resolve-Path $rtl).Path) (Convert-ToForwardPath (Resolve-Path $tb).Path) 2>&1 |
        Tee-Object -FilePath $compileLog
    if ($LASTEXITCODE -ne 0) {
        throw "vlog failed with exit code $LASTEXITCODE"
    }

    $simLog = Join-Path $RunDir 'vsim.log'
    & vsim -c -do "run -all; quit -code 0" work.pcie_v2_axis_mwr_tb 2>&1 |
        Tee-Object -FilePath $simLog
    if ($LASTEXITCODE -ne 0) {
        throw "vsim failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$logText = Get-Content -LiteralPath (Join-Path $RunDir 'vsim.log') -Raw
if ($logText -match 'V2_AXIS_MWR_TB_FAIL') {
    throw "V2 AXIS MWr simulation failed. Logs are in: $RunDir"
}

if ($logText -notmatch 'V2_AXIS_MWR_TB_PASS') {
    throw "V2 AXIS MWr simulation did not report PASS. Logs are in: $RunDir"
}

Write-Host "V2 AXIS MWr simulation passed. Logs are in: $RunDir"

Param(
    [ValidateSet('compile', 'run')]
    [string]$Mode = 'compile',

    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,

    [string]$PdsRoot = 'D:\PDS_2022.2-SP6.4',

    [string]$RunDir = (Join-Path ([System.IO.Path]::GetTempPath()) ("fpga_pcie_dma_ep_modelsim_{0}" -f ([System.Guid]::NewGuid().ToString('N')))),

    [string]$RunTime = 'all',

    [switch]$KeepWave
)

$ErrorActionPreference = 'Stop'

function Convert-ToForwardPath {
    Param([string]$Path)
    return $Path.Replace('\', '/')
}

function Resolve-FilelistEntry {
    Param([string]$Entry)

    $uartPrefix = '../../example_design/rtl/uart2apb_32bit/'
    $dmaPrefix = '../../example_design/rtl/pcie_dma_ctrl/'

    if ($Entry.StartsWith($uartPrefix)) {
        return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot ('hdl/uart2apb_32bit/' + $Entry.Substring($uartPrefix.Length))))
    }

    if ($Entry.StartsWith($dmaPrefix)) {
        return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot ('hdl/pcie_dma_ctrl/' + $Entry.Substring($dmaPrefix.Length))))
    }

    return [System.IO.Path]::GetFullPath((Join-Path $simSourceFull $Entry))
}

if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
    throw "ProjectRoot does not exist: $ProjectRoot"
}

if (-not (Test-Path -LiteralPath $PdsRoot -PathType Container)) {
    throw "PdsRoot does not exist: $PdsRoot"
}

$simSourceDir = Join-Path $ProjectRoot 'ipcore/pcie_test/sim/modelsim'
$sourceFilelist = Join-Path $simSourceDir 'pango_pcie_top_filelist.f'
$sourceDo = Join-Path $simSourceDir 'pango_pcie_top_sim.do'
$pdsSimRoot = Join-Path $PdsRoot 'arch/vendor/pango/verilog/simulation/modelsim10.2c'

if (-not (Test-Path -LiteralPath $sourceFilelist -PathType Leaf)) {
    throw "Missing vendor simulation filelist: $sourceFilelist"
}

if (-not (Test-Path -LiteralPath $sourceDo -PathType Leaf)) {
    throw "Missing vendor simulation do file: $sourceDo"
}

if (-not (Test-Path -LiteralPath $pdsSimRoot -PathType Container)) {
    throw "Missing PDS ModelSim simulation library path: $pdsSimRoot"
}

if (-not (Get-Command vsim -ErrorAction SilentlyContinue)) {
    throw 'ModelSim vsim was not found in PATH'
}

New-Item -ItemType Directory -Path $RunDir -Force | Out-Null
Copy-Item -Path (Join-Path $simSourceDir '*') -Destination $RunDir -Recurse -Force

$generatedFilelist = Join-Path $RunDir 'pango_pcie_top_filelist.generated.f'
$simSourceFull = (Resolve-Path $simSourceDir).Path

$convertedLines = foreach ($line in Get-Content -LiteralPath $sourceFilelist) {
    $trimmed = $line.Trim()

    if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('+') -or $trimmed.StartsWith('//')) {
        $line
        continue
    }

    $absolutePath = Resolve-FilelistEntry $trimmed
    if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf)) {
        throw "Simulation source file does not exist after path remap: $trimmed -> $absolutePath"
    }

    Convert-ToForwardPath $absolutePath
}

Set-Content -LiteralPath $generatedFilelist -Value $convertedLines -Encoding ASCII

$generatedDo = Join-Path $RunDir ("pango_pcie_top_{0}.generated.do" -f $Mode)
$doText = Get-Content -LiteralPath $sourceDo -Raw
$pdsRootForward = Convert-ToForwardPath ((Resolve-Path $PdsRoot).Path)

$doText = $doText.Replace('D:/pds2022_sp6_4/PDS_2022.2-SP6.4', $pdsRootForward)
$doText = $doText.Replace('-f ./pango_pcie_top_filelist.f', '-f ./pango_pcie_top_filelist.generated.f')

if (-not $KeepWave) {
    $doText = $doText.Replace('do pango_pcie_top_wave.do', '# pango_pcie_top_wave.do skipped in batch mode')
}

if ($Mode -eq 'compile') {
    $compileLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($doText -split "`r?`n")) {
        if ($line.TrimStart().StartsWith('vsim ')) {
            break
        }
        $compileLines.Add($line)
    }

    $doText = ($compileLines -join [Environment]::NewLine)
} elseif ($RunTime -ne 'all') {
    $doText = $doText -replace '(?m)^\s*run\s+-all\s*$', "run $RunTime"
}

$doText = "onerror {quit -code 1}" + [Environment]::NewLine + $doText.TrimEnd() + [Environment]::NewLine + "quit -code 0" + [Environment]::NewLine
Set-Content -LiteralPath $generatedDo -Value $doText -Encoding ASCII

Write-Host "Simulation run directory: $RunDir"
Write-Host "Mode: $Mode"
Write-Host "PDS root: $PdsRoot"
if ($Mode -eq 'run') {
    Write-Host "Run time: $RunTime"
}

Push-Location $RunDir
try {
    & vsim -c -do (Split-Path $generatedDo -Leaf) 2>&1 | Tee-Object -FilePath (Join-Path $RunDir 'modelsim_console.log')
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($exitCode -ne 0) {
    Write-Host "ModelSim failed. Logs are in: $RunDir"
    exit $exitCode
}

Write-Host "ModelSim $Mode completed. Logs are in: $RunDir"

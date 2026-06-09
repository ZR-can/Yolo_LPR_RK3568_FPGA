Param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$Failures = @()

function Add-Failure {
    Param([string]$Message)
    $script:Failures += $Message
    Write-Host "[FAIL] $Message"
}

function Add-Ok {
    Param([string]$Message)
    Write-Host "[OK] $Message"
}

function Add-Warn {
    Param([string]$Message)
    Write-Host "[WARN] $Message"
}

if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
    throw "ProjectRoot does not exist: $ProjectRoot"
}

$pds = Join-Path $ProjectRoot 'pcie_dma_test_100h_top.pds'
if (-not (Test-Path -LiteralPath $pds -PathType Leaf)) {
    Add-Failure "Missing wrapper PDS: $pds"
} else {
    $pdsText = Get-Content -LiteralPath $pds -Raw

    if ($pdsText -match '\(_device\s+PG2L100H\)') {
        Add-Ok 'Wrapper PDS targets PG2L100H'
    } else {
        Add-Failure 'Wrapper PDS does not target PG2L100H'
    }

    if ($pdsText -match '\(_package\s+FBG484\)') {
        Add-Ok 'Wrapper PDS targets FBG484 package'
    } else {
        Add-Failure 'Wrapper PDS does not target FBG484 package'
    }

    if ($pdsText -match '\+\s+"pcie_dma_test_100h_top"') {
        Add-Ok 'Wrapper PDS marks pcie_dma_test_100h_top as top'
    } else {
        Add-Failure 'Wrapper PDS top marker is missing'
    }

    if ($pdsText -match '\+\s+"pcie_dma_test"(?!_100h_top")') {
        Add-Failure 'Wrapper PDS still marks vendor pcie_dma_test as top'
    } else {
        Add-Ok 'Wrapper PDS does not mark vendor pcie_dma_test as top'
    }

    if ($pdsText -match 'fdc/pcie_dma_test_100h_top\.fdc') {
        Add-Ok 'Wrapper PDS uses wrapper FDC'
    } else {
        Add-Failure 'Wrapper PDS does not reference wrapper FDC'
    }

    if ($pdsText -match 'hdl/video/pcie_v2_axis_mwr\.v') {
        Add-Ok 'Wrapper PDS includes V2 mailbox/video RTL'
    } else {
        Add-Failure 'Wrapper PDS does not include V2 mailbox/video RTL'
    }
}

$ipIdf = Join-Path $ProjectRoot 'ipcore/pcie_test/pcie_test.idf'
if (Test-Path -LiteralPath $ipIdf -PathType Leaf) {
    $idfText = Get-Content -LiteralPath $ipIdf -Raw
    if ($idfText -match '<device>PG2L100H</device>' -and
        $idfText -match '<package>FBG484</package>' -and
        $idfText -match '<speedgrade>-6</speedgrade>') {
        Add-Ok 'PCIe IP metadata targets PG2L100H/FBG484/-6'
    } else {
        Add-Failure 'PCIe IP metadata does not target PG2L100H/FBG484/-6'
    }
} else {
    Add-Failure "Missing PCIe IP metadata: $ipIdf"
}

$ipWrapper = Join-Path $ProjectRoot 'ipcore/pcie_test/pcie_test.v'
if (Test-Path -LiteralPath $ipWrapper -PathType Leaf) {
    $ipText = Get-Content -LiteralPath $ipWrapper -Raw
    if ($ipText -match 'DEVICE_TYPE\s*=\s*3''b000') {
        Add-Ok 'PCIe IP wrapper is configured as Endpoint'
    } else {
        Add-Failure 'PCIe IP wrapper DEVICE_TYPE is not Endpoint'
    }

    if ($ipText -match 'HSST_LANE_NUM\s*=\s*2') {
        Add-Ok 'PCIe IP wrapper uses 2 HSST lanes'
    } else {
        Add-Failure 'PCIe IP wrapper does not use 2 HSST lanes'
    }

    if ($ipText -match 'VENDOR_ID\s*=\s*16''h0755' -and $ipText -match 'DEVICE_ID\s*=\s*16''h0755') {
        Add-Ok 'PCIe IP wrapper uses PCI ID 0755:0755'
    } else {
        Add-Failure 'PCIe IP wrapper PCI ID is not 0755:0755'
    }

    if ($ipText -match 'BAR0_ENABLED\s*=\s*1''b1' -and
        $ipText -match 'BAR1_ENABLED\s*=\s*1''b1' -and
        $ipText -match 'BAR1_MASK\s*=\s*31''h[1-9a-fA-F][0-9a-fA-F_]*') {
        Add-Ok 'PCIe IP wrapper enables BAR0 and BAR1 with a nonzero BAR1 mask'
    } else {
        Add-Failure 'PCIe IP wrapper does not correctly enable BAR0/BAR1'
    }

    if ($ipText -match 'BAR2_ENABLED\s*=\s*1''b0') {
        Add-Ok 'PCIe IP wrapper keeps BAR2 disabled for V2'
    } else {
        Add-Failure 'PCIe IP wrapper BAR2 should remain disabled for V2'
    }
} else {
    Add-Failure "Missing PCIe IP wrapper: $ipWrapper"
}

$topWrapper = Join-Path $ProjectRoot 'hdl/top/pcie_dma_test_100h_top.v'
$integrationTop = Join-Path $ProjectRoot 'hdl/pcie_dma_test.v'
$fdc = Join-Path $ProjectRoot 'fdc/pcie_dma_test_100h_top.fdc'

if (Test-Path -LiteralPath $topWrapper -PathType Leaf) {
    $topText = Get-Content -LiteralPath $topWrapper -Raw
    if ($topText -match '\btxd\b' -and $topText -match '\brxd\b') {
        Add-Ok 'Wrapper top exposes external UART txd/rxd'
    } else {
        Add-Failure 'Wrapper top does not expose external UART txd/rxd'
    }
} else {
    Add-Failure "Missing wrapper top: $topWrapper"
}

if (Test-Path -LiteralPath $integrationTop -PathType Leaf) {
    $intTopText = Get-Content -LiteralPath $integrationTop -Raw
    if ($intTopText -match 'pcie_v2_axis_mwr' -and
        $intTopText -match 'bar0_wr_en' -and
        $intTopText -match 'dma_axis_slave2_tready') {
        Add-Ok 'Integration top connects V2 BAR0 monitor and MWr injector'
    } else {
        Add-Failure 'Integration top is missing V2 BAR0/MWr integration'
    }
} else {
    Add-Failure "Missing integration top: $integrationTop"
}

if (Test-Path -LiteralPath $fdc -PathType Leaf) {
    $fdcText = Get-Content -LiteralPath $fdc -Raw
    if ($fdcText -match '\{p:txd\}.*\{PAP_IO_LOC\} \{K14\}' -and
        $fdcText -match '\{p:rxd\}.*\{PAP_IO_LOC\} \{H14\}' -and
        $fdcText -match '\{p:txd\}.*\{PAP_IO_VCCIO\} \{1\.8\}' -and
        $fdcText -match '\{p:rxd\}.*\{PAP_IO_VCCIO\} \{1\.8\}' -and
        $fdcText -match '\{p:txd\}.*\{PAP_IO_STANDARD\} \{LVCMOS18\}' -and
        $fdcText -match '\{p:rxd\}.*\{PAP_IO_STANDARD\} \{LVCMOS18\}') {
        Add-Ok 'Wrapper FDC constrains external UART txd/rxd pins'
    } else {
        Add-Failure 'Wrapper FDC does not correctly constrain UART txd/rxd'
    }
} else {
    Add-Failure "Missing wrapper FDC: $fdc"
}

$impl = Join-Path $ProjectRoot 'impl.tcl'
if (Test-Path -LiteralPath $impl -PathType Leaf) {
    $implText = Get-Content -LiteralPath $impl -Raw
    $staleDevice = 'PG2L' + '50H'
    if ($implText -match "BaiduNetdiskDownload|fpga-demos|$staleDevice") {
        Add-Failure "impl.tcl still contains stale vendor paths or $staleDevice target text"
    } else {
        Add-Ok "impl.tcl has no stale vendor absolute paths or $staleDevice target text"
    }

    if ($implText -match 'PG2L100H' -and $implText -match 'pcie_dma_test_100h_top') {
        Add-Ok 'impl.tcl targets PG2L100H wrapper top'
    } else {
        Add-Failure 'impl.tcl does not target PG2L100H wrapper top'
    }
} else {
    Add-Failure "Missing PDS batch script: $impl"
}

$sourceList = Join-Path $ProjectRoot 'scripts/pds_sources.f'
if (-not (Test-Path -LiteralPath $sourceList -PathType Leaf)) {
    Add-Failure "Missing source list: $sourceList"
} else {
    $count = 0
    $missing = @()
    $sourcePaths = @()

    foreach ($line in Get-Content -LiteralPath $sourceList) {
        $entry = (($line -split '\s+#', 2)[0]).Trim()
        if ($entry.Length -eq 0 -or $entry.StartsWith('#')) {
            continue
        }

        $count += 1
        $path = Join-Path $ProjectRoot $entry
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $missing += $entry
        } else {
            $sourcePaths += $path
        }
    }

    if ($missing.Count -eq 0) {
        Add-Ok "All $count source-list entries exist"
    } else {
        Add-Failure "Source list has missing entries: $($missing -join ', ')"
    }

    $sourceListText = Get-Content -LiteralPath $sourceList -Raw
    if ($sourceListText -match 'hdl/video/pcie_v2_axis_mwr\.v') {
        Add-Ok 'Source list includes V2 mailbox/video RTL'
    } else {
        Add-Failure 'Source list does not include V2 mailbox/video RTL'
    }

    if ($missing.Count -eq 0) {
        $modules = @()
        foreach ($path in $sourcePaths) {
            Select-String -LiteralPath $path -Pattern '^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)' |
                ForEach-Object {
                    $modules += [pscustomobject]@{
                        Module = $_.Matches[0].Groups[1].Value
                        Path = $_.Path
                    }
                }
        }

        $dups = $modules | Group-Object Module | Where-Object { $_.Count -gt 1 }
        if ($dups.Count -eq 0) {
            Add-Ok "No duplicate module definitions in source list ($($modules.Count) modules parsed)"
        } else {
            $dupNames = ($dups | ForEach-Object { $_.Name }) -join ', '
            Add-Failure "Duplicate module definitions in source list: $dupNames"
        }
    }
}

$rkTool = Join-Path $ProjectRoot '../src/rk_fpga_pcie_comm_demo.c'
$rkMakefile = Join-Path $ProjectRoot '../src/Makefile'
if (Test-Path -LiteralPath $rkTool -PathType Leaf) {
    $rkToolText = Get-Content -LiteralPath $rkTool -Raw
    if ($rkToolText -match 'V2_CMD_MAGIC' -and
        $rkToolText -match 'V2_OP_VIDEO_START' -and
        $rkToolText -match 'V2_FRAME_MAGIC') {
        Add-Ok 'RK userspace V2 communication demo is present'
    } else {
        Add-Failure 'RK userspace V2 communication demo is missing protocol definitions'
    }
} else {
    Add-Failure "Missing RK V2 userspace tool: $rkTool"
}

if (Test-Path -LiteralPath $rkMakefile -PathType Leaf) {
    $rkMakefileText = Get-Content -LiteralPath $rkMakefile -Raw
    if ($rkMakefileText -match 'dma_memcpy_demo' -and
        $rkMakefileText -match 'rk_fpga_pcie_comm_demo') {
        Add-Ok 'RK Makefile builds legacy benchmark and V2 communication demo'
    } else {
        Add-Failure 'RK Makefile does not build both expected userspace tools'
    }
} else {
    Add-Failure "Missing RK userspace Makefile: $rkMakefile"
}

$redundantPaths = @(
    'ipcore/pcie_test/example_design/rtl/pcie_dma_ctrl',
    'ipcore/pcie_test/example_design/rtl/uart2apb_32bit',
    'ipcore/pcie_test/pcie_test_tmpl.v'
)

$redundantHits = @()
foreach ($entry in $redundantPaths) {
    $path = Join-Path $ProjectRoot $entry
    if (Test-Path -LiteralPath $path) {
        $redundantHits += $entry
    }
}

if ($redundantHits.Count -eq 0) {
    Add-Ok 'No redundant vendor example RTL copies found'
} else {
    Add-Failure "Redundant vendor example RTL still present: $($redundantHits -join ', ')"
}

$staleProjectEntrypoints = @(
    'pcie_dma_test.pds',
    'fdc/pcie_dma_test.fdc',
    'ipcore/pcie_test/pnr/example_design/pango_pcie_top.pds',
    'ipcore/pcie_test/pnr/example_design/pango_pcie_top.backup_1.pds',
    'ipcore/pcie_test/pnr/example_design/pango_pcie_top.fdc',
    'ipcore/pcie_test/pnr/core_only/pcie_test.pds',
    'ipcore/pcie_test/pnr/core_only/pcie_test.fdc'
)

$staleHits = @()
foreach ($entry in $staleProjectEntrypoints) {
    $path = Join-Path $ProjectRoot $entry
    if (Test-Path -LiteralPath $path) {
        $staleHits += $entry
    }
}

if ($staleHits.Count -eq 0) {
    Add-Ok 'No stale non-wrapper PDS/FDC entrypoints found'
} else {
    Add-Failure "Stale non-wrapper PDS/FDC entrypoints still present: $($staleHits -join ', ')"
}

$simFilelist = Join-Path $ProjectRoot 'ipcore/pcie_test/sim/modelsim/pango_pcie_top_filelist.f'
if (Test-Path -LiteralPath $simFilelist -PathType Leaf) {
    $simFilelistText = Get-Content -LiteralPath $simFilelist -Raw
    if ($simFilelistText -match '\.\./\.\./example_design/rtl/(pcie_dma_ctrl|uart2apb_32bit)') {
        Add-Failure 'ModelSim filelist still points at removed duplicate DMA/UART example RTL'
    } else {
        Add-Ok 'ModelSim filelist uses canonical DMA/UART RTL sources'
    }
} else {
    Add-Failure "Missing ModelSim filelist: $simFilelist"
}

$generatedDirs = @(
    'compile',
    'constraint_check',
    'device_map',
    'generate_bitstream',
    'generate_netlist',
    'ip_backup',
    'log',
    'logbackup',
    'place_route',
    'report_power',
    'report_timing',
    'synthesize'
)
$dirHits = Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Force -Directory |
    Where-Object { $generatedDirs -contains $_.Name }

if ($dirHits.Count -eq 0) {
    Add-Ok 'No generated PDS output directories found'
} else {
    Add-Failure "Generated output directories found: $($dirHits.FullName -join ', ')"
}

$generatedPatterns = @('*.log', '*.rpt', '*.sbit', '*.bit', '*.db', '*.jou', '*.str', '*.fsdb', '*.vcd', '*.lock')
$fileHits = Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Force -File |
    Where-Object {
        $name = $_.Name
        ($name -eq '.last_generated') -or
        ($name -eq '.settings') -or
        (($generatedPatterns | Where-Object { $name -like $_ }).Count -gt 0)
    }

if ($fileHits.Count -eq 0) {
    Add-Ok 'No generated artifact file patterns found'
} else {
    Add-Failure "Generated artifact files found: $($fileHits.FullName -join ', ')"
}

if ($Failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Project check failed.'
    exit 1
}

Write-Host ''
Write-Host 'Project check passed.'

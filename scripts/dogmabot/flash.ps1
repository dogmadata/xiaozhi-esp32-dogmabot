# Flash xiaozhi-esp32-dogmabot firmware via Windows PowerShell.
# Auto-discovers the binary in this order:
#   1. -Bin <path>  (explicit; accepts .bin OR .zip release archive)
#   2. <repoRoot>/releases/v*.zip  (latest by mtime — what CI produces)
#   3. <repoRoot>/build/merged-binary.bin  (local IDF build)
#   4. <cwd>/merged-binary.bin
#   5. <cwd>/v*.zip
#
# Usage:
#   .\flash.ps1                                # full flash from latest zip/bin
#   .\flash.ps1 -AppOnly                       # flash ONLY app partition (build/xiaozhi.bin at 0x20000)
#                                              # ~3x faster, only works with a local build
#   .\flash.ps1 -Erase                         # erase whole flash first
#   .\flash.ps1 -Bin C:\Downloads\v2.2.6_sp-esp32-s3-1.28-box.zip
#   .\flash.ps1 -Port COM7 -Baud 921600
# Requisitos: Python 3 + esptool no PATH (instalado on-demand se faltar).

[CmdletBinding()]
param(
    [string]$Bin = "",
    [string]$Port = "",
    [int]$Baud = 460800,
    [switch]$Erase,
    [switch]$Monitor,
    [switch]$AppOnly
)

$ErrorActionPreference = "Stop"

# scripts/dogmabot/  →  ../..
# Use ProviderPath so paths under \\wsl.localhost\... or other PSDrives don't
# inherit the "Microsoft.PowerShell.Core\FileSystem::" prefix, which esptool
# (and other native tools) reject as "No such file or directory".
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath

function Get-EspPorts {
    $known = @{
        "VID_303A" = "Espressif USB-Serial/JTAG (nativo ESP32-S3)"
        "VID_10C4" = "Silicon Labs CP210x"
        "VID_1A86" = "WCH CH340/CH9102"
        "VID_0403" = "FTDI"
    }
    $devs = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match "COM\d+" }
    $result = foreach ($d in $devs) {
        $com = ([regex]::Match($d.FriendlyName, "COM\d+")).Value
        $vid = ($known.Keys | Where-Object { $d.InstanceId -match $_ } | Select-Object -First 1)
        [pscustomobject]@{
            Port    = $com
            Name    = $d.FriendlyName
            Vendor  = if ($vid) { $known[$vid] } else { "desconhecido" }
            IsEsp   = [bool]$vid
        }
    }
    $result | Sort-Object -Property @{Expression="IsEsp";Descending=$true}, Port
}

function Select-Port {
    $ports = Get-EspPorts
    if (-not $ports) {
        Write-Host "Nenhuma porta COM detectada. Conecte a placa via USB e tente de novo." -ForegroundColor Red
        Write-Host "Dica: no Windows, a placa precisa estar atachada ao SO host (não ao WSL). Se estiver com usbipd, rode 'usbipd detach --busid <X-Y>'." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "Portas COM disponiveis:" -ForegroundColor Cyan
    for ($i = 0; $i -lt $ports.Count; $i++) {
        $p = $ports[$i]
        $tag = if ($p.IsEsp) { "[ESP?]" } else { "      " }
        Write-Host ("  [{0}] {1} {2}  - {3} ({4})" -f $i, $tag, $p.Port, $p.Name, $p.Vendor)
    }
    $default = ($ports | Where-Object IsEsp | Select-Object -First 1)
    $prompt = "Escolha o indice"
    if ($default) { $prompt += " (Enter = $($default.Port))" }
    $sel = Read-Host $prompt
    if (-not $sel -and $default) { return $default.Port }
    if ($sel -match '^\d+$' -and [int]$sel -lt $ports.Count) { return $ports[[int]$sel].Port }
    Write-Host "Selecao invalida." -ForegroundColor Red; exit 1
}

# ──────────────────────────────────────────────────────────────────────────
# Resolve binary
# ──────────────────────────────────────────────────────────────────────────

function Find-Binary {
    if ($Bin) {
        if (-not (Test-Path $Bin)) {
            Write-Host "Arquivo nao encontrado: $Bin" -ForegroundColor Red; exit 1
        }
        return (Resolve-Path $Bin).ProviderPath
    }

    $candidates = @()
    # 1. CI artifacts
    $relDir = Join-Path $RepoRoot "releases"
    if (Test-Path $relDir) {
        $candidates += Get-ChildItem $relDir -Filter "v*.zip" -ErrorAction SilentlyContinue
    }
    # 2. Local build
    $localBuild = Join-Path $RepoRoot "build\merged-binary.bin"
    if (Test-Path $localBuild) {
        $candidates += Get-Item $localBuild
    }
    # 3. cwd fallbacks
    $cwd = $PWD.ProviderPath
    $cwdBin = Join-Path $cwd "merged-binary.bin"
    if (Test-Path $cwdBin) { $candidates += Get-Item $cwdBin }
    $candidates += Get-ChildItem $cwd -Filter "v*.zip" -ErrorAction SilentlyContinue

    $best = $candidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $best) {
        Write-Host "Nao encontrei merged-binary.bin nem um zip de release." -ForegroundColor Red
        Write-Host "  Esperado em: $relDir, $localBuild, ou cwd." -ForegroundColor Yellow
        Write-Host "  Ou passe -Bin <caminho> explicitamente." -ForegroundColor Yellow
        exit 1
    }
    # Normalize via Convert-Path to strip any PSDrive/provider prefix.
    return (Convert-Path $best.FullName)
}

function Resolve-MergedBin {
    param([string]$Path)
    if ($Path -like "*.zip") {
        Write-Host "Extraindo merged-binary.bin de: $(Split-Path $Path -Leaf)" -ForegroundColor Cyan
        $tempDir = Join-Path $env:TEMP "dogmabot-flash-$([Guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
        # ESP-IDF release.py zip ships exactly merged-binary.bin at the root.
        Expand-Archive -Path $Path -DestinationPath $tempDir -Force
        $extracted = Get-ChildItem $tempDir -Filter "merged-binary.bin" -Recurse |
            Select-Object -First 1
        if (-not $extracted) {
            Write-Host "Zip nao contem merged-binary.bin." -ForegroundColor Red; exit 1
        }
        return $extracted.FullName
    }
    return $Path
}

# App-only mode: skip merged-binary, flash just xiaozhi.bin at 0x20000.
# Saves ~2/3 of the transfer when iterating on UI/code without touching the
# bootloader, partition table or assets partition.
$appOnlyBin = $null
if ($AppOnly) {
    if ($Bin) {
        if (-not (Test-Path $Bin)) {
            Write-Host "Arquivo nao encontrado: $Bin" -ForegroundColor Red; exit 1
        }
        $appOnlyBin = (Resolve-Path $Bin).ProviderPath
    } else {
        $appCandidate = Join-Path $RepoRoot "build\xiaozhi.bin"
        if (-not (Test-Path $appCandidate)) {
            Write-Host "AppOnly precisa de build/xiaozhi.bin (build local do IDF)." -ForegroundColor Red
            Write-Host "  Esperado em: $appCandidate" -ForegroundColor Yellow
            Write-Host "  Rode 'idf.py build' antes, ou passe -Bin <caminho-do-xiaozhi.bin>." -ForegroundColor Yellow
            exit 1
        }
        $appOnlyBin = (Convert-Path $appCandidate)
    }
    Write-Host "Usando (app only): $appOnlyBin" -ForegroundColor DarkGray
} else {
    $source = Find-Binary
    $mergedBin = Resolve-MergedBin -Path $source
    Write-Host "Usando: $source" -ForegroundColor DarkGray
}

# ──────────────────────────────────────────────────────────────────────────
# Garantir esptool
# ──────────────────────────────────────────────────────────────────────────

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
if (-not $py) {
    Write-Host "Python nao encontrado no PATH. Instale Python 3 (https://python.org) e rode 'pip install esptool'." -ForegroundColor Red
    exit 1
}
& $py.Source -m esptool version > $null 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "esptool nao instalado. Instalando via pip..." -ForegroundColor Yellow
    & $py.Source -m pip install --user esptool
    if ($LASTEXITCODE -ne 0) { Write-Host "Falha ao instalar esptool." -ForegroundColor Red; exit 1 }
}

# ──────────────────────────────────────────────────────────────────────────
# Flash
# ──────────────────────────────────────────────────────────────────────────

if (-not $Port) { $Port = Select-Port }
Write-Host "Usando porta $Port a $Baud bps" -ForegroundColor Green

if ($Erase) {
    if ($AppOnly) {
        Write-Host "Ignorando -Erase em modo AppOnly (so a particao app sera gravada)." -ForegroundColor Yellow
    } else {
        Write-Host "Apagando flash..." -ForegroundColor Yellow
        & $py.Source -m esptool --chip esp32s3 -p $Port -b $Baud erase_flash
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

if ($AppOnly) {
    Write-Host "Gravando xiaozhi.bin em 0x20000 (so a particao app)..." -ForegroundColor Cyan
    & $py.Source -m esptool --chip esp32s3 -p $Port -b $Baud `
        --before default_reset --after hard_reset write_flash `
        --flash_mode dio --flash_size 16MB --flash_freq 80m `
        0x20000 $appOnlyBin
} else {
    Write-Host "Gravando merged-binary em 0x0..." -ForegroundColor Cyan
    & $py.Source -m esptool --chip esp32s3 -p $Port -b $Baud `
        --before default_reset --after hard_reset write_flash `
        --flash_mode dio --flash_size 16MB --flash_freq 80m `
        0x0 $mergedBin
}
if ($LASTEXITCODE -ne 0) { Write-Host "Falha na gravacao." -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "Gravacao concluida." -ForegroundColor Green

if ($Monitor) {
    Write-Host "Abrindo monitor serial (Ctrl+C para sair)..." -ForegroundColor Cyan
    & $py.Source -m serial.tools.miniterm $Port 115200
}

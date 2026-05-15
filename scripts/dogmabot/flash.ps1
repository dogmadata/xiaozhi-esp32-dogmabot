# Flash Spotpear ESP32-S3-1.28-BOX (xiaozhi pt-PT) — rodar no Windows PowerShell
# Requisitos: Python 3 + pip install esptool. Executar do mesmo diretório do build/.

[CmdletBinding()]
param(
    [string]$Port = "",
    [int]$Baud = 460800,
    [switch]$Erase,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

function Get-EspPorts {
    # Identifica COMs USB. ESP32-S3 nativo usa VID 303A; CP210x (10C4), CH340 (1A86), FTDI (0403).
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
        # Dica para WSL/usbipd
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

# Validar binarios
$required = @("merged-binary.bin")
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $PSScriptRoot $f))) {
        Write-Host "Arquivo nao encontrado: $f. Rode o build antes (idf.py build && idf.py merge-bin)." -ForegroundColor Red
        exit 1
    }
}

# Garantir esptool
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

if (-not $Port) { $Port = Select-Port }
Write-Host "Usando porta $Port a $Baud bps" -ForegroundColor Green

if ($Erase) {
    Write-Host "Apagando flash..." -ForegroundColor Yellow
    & $py.Source -m esptool --chip esp32s3 -p $Port -b $Baud erase_flash
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Gravando merged-binary.bin em 0x0..." -ForegroundColor Cyan
& $py.Source -m esptool --chip esp32s3 -p $Port -b $Baud `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_size 16MB --flash_freq 80m `
    0x0 merged-binary.bin
if ($LASTEXITCODE -ne 0) { Write-Host "Falha na gravacao." -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "Gravacao concluida." -ForegroundColor Green

if ($Monitor) {
    Write-Host "Abrindo monitor serial (Ctrl+C para sair)..." -ForegroundColor Cyan
    # Sem o miniterm do IDF aqui — sugerir alternativas
    Write-Host "Use um terminal serial a 115200 8N1 (PuTTY, Tera Term, ou 'python -m serial.tools.miniterm $Port 115200')." -ForegroundColor Yellow
    & $py.Source -m serial.tools.miniterm $Port 115200
}

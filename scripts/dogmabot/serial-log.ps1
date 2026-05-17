# Captura log serial do ESP32 em arquivo + tela.
# O log vai por padrao para o diretorio onde voce executou o script (cwd),
# nao para a pasta do proprio script — assim nao polui o repo.
#
# Uso:
#   .\serial-log.ps1                                  # detecta COM, log na cwd
#   .\serial-log.ps1 -Port COM7
#   .\serial-log.ps1 -Reset                           # pulsa DTR/RTS antes
#   .\serial-log.ps1 -LogFile C:\logs\boot.txt
# Ctrl+C para parar.

[CmdletBinding()]
param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$LogFile = "",
    [switch]$Reset
)

$ErrorActionPreference = "Stop"

function Get-EspPorts {
    $known = @{
        "VID_303A" = "Espressif USB-Serial/JTAG"
        "VID_10C4" = "CP210x"
        "VID_1A86" = "CH340/CH9102"
        "VID_0403" = "FTDI"
    }
    $devs = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match "COM\d+" }
    $r = foreach ($d in $devs) {
        $com = ([regex]::Match($d.FriendlyName, "COM\d+")).Value
        $vid = ($known.Keys | Where-Object { $d.InstanceId -match $_ } | Select-Object -First 1)
        [pscustomobject]@{
            Port  = $com
            Name  = $d.FriendlyName
            IsEsp = [bool]$vid
        }
    }
    $r | Sort-Object -Property @{Expression="IsEsp";Descending=$true}, Port
}

function Select-Port {
    $ports = Get-EspPorts
    if (-not $ports) { Write-Host "Nenhuma COM detectada." -ForegroundColor Red; exit 1 }
    for ($i=0; $i -lt $ports.Count; $i++) {
        $p = $ports[$i]; $tag = if ($p.IsEsp){"[ESP?]"}else{"      "}
        Write-Host ("  [{0}] {1} {2}  - {3}" -f $i,$tag,$p.Port,$p.Name)
    }
    $def = ($ports | Where-Object IsEsp | Select-Object -First 1)
    $sel = Read-Host ("Escolha indice" + $(if ($def) { " (Enter = $($def.Port))" }))
    if (-not $sel -and $def) { return $def.Port }
    if ($sel -match '^\d+$' -and [int]$sel -lt $ports.Count) { return $ports[[int]$sel].Port }
    Write-Host "Selecao invalida." -ForegroundColor Red; exit 1
}

if (-not $Port) { $Port = Select-Port }
if (-not $LogFile) {
    $LogFile = "serial-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date)
}
# Relative paths resolve against the user's cwd, not $PSScriptRoot.
# Use ProviderPath — (Get-Location).Path can include the "FileSystem::" provider
# prefix in some shells, which StreamWriter rejects with NotSupportedException.
if (-not [System.IO.Path]::IsPathRooted($LogFile)) {
    $cwd = $PWD.ProviderPath
    if (-not $cwd) { $cwd = [Environment]::CurrentDirectory }
    $LogFile = Join-Path $cwd $LogFile
}
$LogFile = [System.IO.Path]::GetFullPath($LogFile)

Write-Host "Porta: $Port @ $Baud  Log: $LogFile" -ForegroundColor Green
Write-Host "Ctrl+C para parar." -ForegroundColor Yellow

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.NewLine = "`n"
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()

if ($Reset) {
    Write-Host "Resetando device..." -ForegroundColor Cyan
    $sp.DtrEnable = $true; $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 50
    $sp.DtrEnable = $false; $sp.RtsEnable = $false
}

"=== $(Get-Date -Format o) | $Port @ $Baud ===" | Out-File -FilePath $LogFile -Encoding utf8

try {
    $writer = [System.IO.StreamWriter]::new($LogFile, $true, [System.Text.Encoding]::UTF8)
    $writer.AutoFlush = $true
    while ($true) {
        try {
            $line = $sp.ReadLine()
            if ($line) {
                Write-Host $line
                $writer.WriteLine($line)
            }
        } catch [TimeoutException] {
            # sem dados, segue
        }
    }
} finally {
    if ($writer) { $writer.Close() }
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    Write-Host "`nLog salvo em: $LogFile" -ForegroundColor Green
}

# DogmaBot PowerShell helpers

Scripts auxiliares para gravação e captura de log do firmware
`xiaozhi-esp32-dogmabot` a partir do Windows (úteis especialmente em
WSL, onde o `idf.py flash` não enxerga a porta serial diretamente).

## `flash.ps1`

Grava o `merged-binary.bin` (zip do release CI ou build local) na placa.

```powershell
# Auto-descobre o .bin/.zip mais recente em releases/, build/ ou cwd.
.\flash.ps1

# Apaga a flash antes (recomendado na primeira vez ou após upgrade
# de versão com layout de partição diferente):
.\flash.ps1 -Erase

# Caminho explícito (zip de release ou .bin solto):
.\flash.ps1 -Bin C:\Downloads\v2.2.6_sp-esp32-s3-1.28-box.zip

# Forçar porta / baud / abrir monitor após gravar:
.\flash.ps1 -Port COM7 -Baud 921600 -Monitor
```

Ordem de auto-descoberta (mais recente vence):

1. `<repo>/releases/v*.zip` — saída do `python scripts/release.py`
   ou do CI workflow (`release.yml`).
2. `<repo>/build/merged-binary.bin` — após `idf.py build && idf.py merge-bin`.
3. `<cwd>/merged-binary.bin` ou `<cwd>/v*.zip` — se você baixou
   manualmente o zip da GitHub Release.

Requisitos: Python 3 no PATH. O script instala `esptool` via `pip
install --user` na primeira vez se faltar.

## `serial-log.ps1`

Captura o boot log do device em arquivo + tela.

```powershell
# Detecta COM, grava em serial-YYYYMMDD-HHmmss.log na cwd:
.\serial-log.ps1

# Resetar o device via DTR/RTS antes (para pegar o boot do começo):
.\serial-log.ps1 -Reset

# Forçar porta ou arquivo de saída:
.\serial-log.ps1 -Port COM7 -LogFile C:\logs\boot.txt
```

Ctrl+C encerra. Por padrão o log vai para o diretório onde você
**executou** o script — não para a pasta do próprio script — para
não poluir o repo.

## Identificação automática de portas

Ambos os scripts usam `Get-PnpDevice -Class Ports` e marcam com `[ESP?]`
qualquer COM com VID conhecido de chips USB-serial de ESP32:

| VID | Chip |
|---|---|
| `303A` | USB-Serial/JTAG nativo do ESP32-S3 |
| `10C4` | Silicon Labs CP210x |
| `1A86` | WCH CH340 / CH9102 |
| `0403` | FTDI |

Se nada aparece, normalmente é porque a placa está atachada ao WSL via
`usbipd` — solte com `usbipd detach --busid <X-Y>` no PowerShell admin
do Windows antes de rodar os scripts.

## Publicando um release (CI → GitHub Release → DogmaBotServer)

O workflow `.github/workflows/release.yml` builda os boards
customizados (hoje `sp-esp32-s3-1.28-box` e
`waveshare/esp32-s3-rlcd-4.2`) e anexa os zips `v{ver}_{board}.zip`
em uma GitHub Release sempre que uma tag `v*` é empurrada.

Procedimento a partir do branch `feat/dogmabot-customizations`:

```bash
git checkout feat/dogmabot-customizations
git pull --rebase
git tag v2.2.6
git push origin v2.2.6
```

Se a tag `v2.2.6` já existe (estamos reusando o mesmo `PROJECT_VER`):

```bash
# remover localmente e no remoto
git tag -d v2.2.6
git push origin :refs/tags/v2.2.6
# apagar a GitHub Release antiga pela UI ou:
gh release delete v2.2.6 -y
# retag + push
git tag v2.2.6
git push origin v2.2.6
```

No DogmaBotServer, **Admin → Firmware → Sync from upstream**, escolha
o repo `dogmadata/xiaozhi-esp32-dogmabot`, a tag, o asset
(`v2.2.6_{board}.zip`) e o board alvo. O server desempacota
`merged-binary.bin`, recorta a partição APP e arquiva como
`{board}_2.2.6.bin` no catálogo. Devices que apontam para esse
server recebem a URL OTA na próxima requisição
`POST /xiaozhi/ota/`.

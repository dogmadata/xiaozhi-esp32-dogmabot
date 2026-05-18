# DogmaBot PowerShell helpers

Scripts auxiliares para gravação e captura de log do firmware
`xiaozhi-esp32-dogmabot` a partir do Windows (úteis especialmente em
WSL, onde o `idf.py flash` não enxerga a porta serial diretamente).

## `build.sh`

Build interativo do firmware via docker (`espressif/idf:v5.5.2`).
Rodar no WSL/Linux — wrapper de `scripts/release.py`.

```bash
cd ~/sources/ESP32/Firmware/xiaozhi-esp32-dogmabot/scripts/dogmabot
./build.sh
```

O script lista os boards (os do DogmaBot — `sp-esp32-s3-1.28-box` e
`waveshare/esp32-s3-rlcd-4.2` — aparecem no topo marcados com `*`),
pergunta qual buildar (número ou nome, Enter = primeiro), oferece
limpar `build/` + zips antigos do board, e roda o docker.

Saídas:

- `releases/v{ver}_{board}.zip` — pacote completo (merged-binary).
- `build/xiaozhi.bin` — app only, use com `flash.ps1 -AppOnly`.
- `build/merged-binary.bin` — full flash.

Sobrescrever a imagem IDF (raro):

```bash
IDF_IMAGE=espressif/idf:v5.6 ./build.sh
```

## `flash.ps1`

Grava o `merged-binary.bin` (zip do release CI ou build local) na placa.

```powershell
# Lista todos os candidatos (.bin/.zip em releases/, build/ e cwd) e
# pergunta qual gravar. Enter usa o mais recente. Se só existe um, grava
# direto sem perguntar.
.\flash.ps1

# Sempre perguntar (mesmo com 1 candidato):
.\flash.ps1 -Pick

# Sempre pegar o mais recente sem perguntar (comportamento antigo):
.\flash.ps1 -Latest

# Apaga a flash antes (recomendado na primeira vez ou após upgrade
# de versão com layout de partição diferente):
.\flash.ps1 -Erase

# Caminho explícito (zip de release ou .bin solto):
.\flash.ps1 -Bin C:\Downloads\v2.2.6_sp-esp32-s3-1.28-box.zip

# Forçar porta / baud / abrir monitor após gravar:
.\flash.ps1 -Port COM7 -Baud 921600 -Monitor

# Gravar APENAS a partição da app (xiaozhi.bin em 0x20000):
.\flash.ps1 -AppOnly
```

Ordem de auto-descoberta (mais recente vence):

1. `<repo>/releases/v*.zip` — saída do `python scripts/release.py`
   ou do CI workflow (`release.yml`).
2. `<repo>/build/merged-binary.bin` — após `idf.py build && idf.py merge-bin`.
3. `<cwd>/merged-binary.bin` ou `<cwd>/v*.zip` — se você baixou
   manualmente o zip da GitHub Release.

Requisitos: Python 3 no PATH. O script instala `esptool` via `pip
install --user` na primeira vez se faltar.

### `-AppOnly` — gravação rápida (só a partição da app)

Por padrão o script grava o `merged-binary.bin`, que cobre todas as
partições (bootloader, partition-table, ota_data, app, assets) a
partir de `0x0` — ~9 MB. Com `-AppOnly`, grava apenas o
`xiaozhi.bin` (~2.8 MB) em `0x20000` (`ota_0`) **e** o
`ota_data_initial.bin` (8 KB) em `0xd000`. A gravação do `ota_data`
"rearma" o slot 0 como ativo — sem isso, se o device já tinha
recebido um OTA do servidor e estava bootando do `ota_1`, o
bootloader continuaria no slot antigo e suas mudanças locais não
apareceriam. A NVS em `0x9000` (Wi-Fi, claim) fica intacta.

```powershell
# Pega build/xiaozhi.bin do build local:
.\flash.ps1 -AppOnly

# Combinável com -Port e -Monitor:
.\flash.ps1 -AppOnly -Port COM7 -Monitor

# Caminho explícito (útil se o .bin estiver fora de build/):
.\flash.ps1 -AppOnly -Bin C:\path\to\xiaozhi.bin
```

**Quando usar:**

- Iteração no código C++ / UI / configs de partição **app**. ~3× mais
  rápido que o full flash.

**Quando NÃO usar:**

- Mudou conteúdo da partição **assets** (`main/assets/`, GIFs,
  fontes, wake-word, locale): você precisa regravar o
  `generated_assets.bin` em `0x800000`, então faça flash normal
  (sem `-AppOnly`).
- Mudou o **layout de partições** (`partitions/v2/*.csv`) ou o
  **bootloader**: full flash + `-Erase`.
- Quer **preservar a senha do Wi-Fi e o claim do device**:
  `-AppOnly` preserva a partição NVS em `0x9000` (full flash
  zera a NVS porque o `merge_bin -f raw` preenche o gap entre
  partition-table e `ota_data` com `0xFF`).
- Você passou `-Bin` apontando para um `xiaozhi.bin` fora de
  `build/`: o reset de `ota_data` é pulado (com aviso). Se o device
  tinha sido OTA'do, faça full flash uma vez. Quando você usa
  `-AppOnly` sem `-Bin`, o `build/ota_data_initial.bin` é encontrado
  automaticamente e o slot é resetado.

**Limitações:**

- Não funciona com zip de release — esse contém só o
  `merged-binary.bin`, e fatiar a partição app dele exigiria
  parsear a tabela de partições. Use **build local**
  (`idf.py build` → produz `build/xiaozhi.bin`).
- A flag `-Erase` é ignorada em modo `-AppOnly` (faz sentido só
  no full flash).

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

> Documentação completa do pipeline (incluindo adicionar boards
> novos, mergear upstream, reverter releases ruins): veja
> [`docs/dogmabot-release.md`](../../docs/dogmabot-release.md).

O workflow `.github/workflows/release.yml` builda os boards
customizados (hoje `sp-esp32-s3-1.28-box` e
`waveshare/esp32-s3-rlcd-4.2`) e anexa os zips `v{ver}_{board}.zip`
em uma GitHub Release sempre que uma tag `v*` é empurrada.

Procedimento a partir do branch `dogmabot`:

```bash
git checkout dogmabot
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

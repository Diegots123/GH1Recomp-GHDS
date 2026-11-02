# GH1 Recomp

Recompilação nativa para PC do **Guitar Hero (PS2, USA, SLUS-21224, v1.00)**, feita a partir do **seu próprio disco**.
O executável do jogo é traduzido para C++ e compilado para Windows (ou Linux). O que roda é o motor do próprio GH1, sem mods: mesma jogabilidade e mesmo timing do PS2, com loads mais rápidos, resolução maior e guitarras atuais.

*English below.*

> **Este repositório não contém nada do jogo.** Não há executável, áudio, vídeo, texturas nem código recompilado aqui, e as Releases não trazem o exe.
> Para jogar, cada pessoa compila o jogo a partir da imagem do próprio disco (Redump, Guitar Hero (USA) v1.00).
> O exe gerado contém código traduzido do seu disco: **é só para você, não o distribua.**
>
> Projeto não oficial, sem ligação com a Harmonix, a RedOctane ou a Activision. Guitar Hero é marca da Activision.

## O que funciona

- Boot, vídeos, menus, Quick Play e Career com músicas inteiras, e saves no cartão de memória (`memcards\`).
- Vídeo pelo GS do PCSX2 em OpenGL (resolução interna 3× por padrão, tela cheia com F11 ou Alt+Enter); VU1 pelo microVU do PCSX2.
- Controles: guitarras atuais (Xbox 360, PS3, Wii por raphnet, Rock Band 4, Riffmaster, World Tour de PC), guitarra e DualShock 2 de PS2 por adaptador USB, e teclado (configurado com F1 no menu principal).
- Só abre com a imagem idêntica à do Redump: nada modificado.

Controles e configurações em detalhe: [`dist/windows/LEIA-ME.txt`](dist/windows/LEIA-ME.txt). Notas de desenvolvimento: [`GH1_PROGRESS.md`](GH1_PROGRESS.md).

## Como compilar (exe de Windows)

O exe é compilado no Linux (no Windows, pelo WSL) com o clang-cl, contra a biblioteca C e o SDK do Windows que o [xwin](https://github.com/Jake-Shadle/xwin) baixa da Microsoft. A primeira compilação leva uns 20 minutos (mais, se a internet for lenta) e usa uns 5 GB de disco.

**1. Ubuntu 24.04.** No Windows, abra o PowerShell como administrador e rode `wsl --install -d Ubuntu-24.04`, reinicie e crie seu usuário no Ubuntu. Depois, no terminal do Ubuntu:

```sh
sudo apt update
sudo apt install -y git cmake ninja-build python3 build-essential pkg-config curl \
    clang-20 clang-tools-20 lld-20 llvm-20
```

**2. O SDK do Windows, pelo xwin.** O `--accept-license` aceita a licença da Microsoft para esses arquivos:

```sh
curl -LO https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
tar xf xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
./xwin-0.6.5-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --use-winsysroot-style --output ~/xwin-sdk
```

**3. Este repositório e a sua ISO.** O disco do Windows aparece no Ubuntu em `/mnt/c` (por exemplo, `C:\Jogos\Guitar Hero (USA).iso` é `/mnt/c/Jogos/Guitar Hero (USA).iso`):

```sh
git clone https://github.com/Diegots123/GH1Recomp-GHDS.git
cd GH1Recomp-GHDS
python3 tools/gh1.py verify "/mnt/c/Jogos/Guitar Hero (USA).iso"      # confere com o Redump
python3 tools/gh1.py build --windows ~/xwin-sdk "/mnt/c/Jogos/Guitar Hero (USA).iso"
```

O `build` lê só o executável do disco (`SLUS_212.24`), confere o SHA-1, recompila, baixa o PCSX2 e o SDL3 nas versões fixadas e compila. Cada etapa grava um log em `build/`. Se a compilação parar por falta de memória, limite os processos em paralelo e rode o mesmo comando de novo: `CMAKE_BUILD_PARALLEL_LEVEL=2 python3 tools/gh1.py build ...`.

**4. Jogar.** O resultado é `build/GH1Recomp-win64.zip`. Copie para o Windows (`cp build/GH1Recomp-win64.zip /mnt/c/Jogos/`), extraia, coloque a ISO na pasta `iso` e abra o `GH1Recomp.exe`. É preciso Windows 10 ou 11 de 64 bits e placa de vídeo com OpenGL 4.3.

### Linux

Além dos pacotes do passo 1, instale as bibliotecas de janela, som e vídeo e compile sem o `--windows`:

```sh
sudo apt install -y libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgl-dev libegl-dev libasound2-dev libpulse-dev libdbus-1-dev libudev-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
python3 tools/gh1.py build "Guitar Hero (USA).iso"
build/runtime/ps2xRuntime/ps2EntryRunner "Guitar Hero (USA).iso"
```

## Como funciona

- O [PS2Recomp](https://github.com/ran-j/PS2Recomp) traduz cada função do executável do GH1 (MIPS R5900) para C++, e o runtime dele faz o papel do kernel e do IOP do PS2.
- Os nomes das funções (`config/gh1-retail.symbols`, só endereços e nomes) vêm da lista pública do GH2 do GHClassic, transferidos por comparação de código (`tools/gh1_names.py`).
- Só funções das bibliotecas da Sony e da libc com identidade provada são trocadas por código do runtime (`tools/gh1_stub_gate.py`); todo o código de jogo é o do GH1.
- `pcsx2gs/` compila sozinho o GS e o microVU do PCSX2 (sem modificar o código deles) e liga ao jogo uma janela SDL3, com os controles e o som.
- `gh1/` é a camada do GH1: o servidor de som (o módulo de synth do GHClassic), o áudio, os controles, o launcher e os traces de diagnóstico.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `gh1/` | camada do GH1 ligada ao runtime |
| `pcsx2gs/` | GS e microVU do PCSX2 fora do emulador; janela, controles e teclado (SDL3) |
| `src/` | módulo de synth do GHClassic e cabeçalhos que ele usa |
| `lib/PS2Recomp/` | recompilador e runtime (fork com as mudanças do GH1) |
| `tools/` | `gh1.py` (verificar, compilar), nomes das funções, empacotamento, toolchain do Windows |
| `config/` | nomes das funções do GH1 e do GH2, e a lista de funções que nunca viram stub |
| `dist/windows/` | o que vai no zip: `GH1Recomp.ini`, `LEIA-ME.txt`, `gamecontrollerdb.txt` |

## Créditos e licenças

- **[GH Classic](https://github.com/noahbaxter/GHClassic)** (noahbaxter), GPL-3.0: a base deste projeto. O módulo de synth, as ferramentas de nomes e símbolos, a lista de nomes do GH2 e o fork do PS2Recomp vêm de lá.
- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** (ran-j e colaboradores), GPL-3.0: recompilador e runtime. `lib/PS2Recomp` é o fork do GHClassic com as mudanças do GH1; os módulos de PS Vita (`.suprx`) ficaram de fora.
- **[PCSX2](https://github.com/PCSX2/pcsx2)**, GPL-3.0-or-later: GS e microVU, baixados na compilação (commit fixado em `tools/gh1.py`) e usados sem modificação.
- **[SDL3](https://github.com/libsdl-org/SDL)** (zlib), **[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)** (zlib), **[raylib](https://github.com/raysan5/raylib)** (zlib), **[FFmpeg](https://ffmpeg.org)** (LGPL-2.1-or-later), e as bibliotecas que o PCSX2 e o PS2Recomp trazem.

Este projeto é distribuído sob a **GPL-3.0** ([`LICENSE`](LICENSE)). O zip gerado inclui todas as licenças em `LICENCAS.txt`.

---

## English

A native PC recompilation of **Guitar Hero (PS2, USA, SLUS-21224, v1.00)**, built from **your own disc**. The game's executable is translated to C++ and compiled for Windows (or Linux). It runs GH1's own engine with no mods: the PS2's gameplay and timing, with faster loads, higher resolution and modern guitars.

> **This repository contains nothing from the game**: no executable, audio, video, textures or recompiled code, and the Releases carry no exe. Everyone builds the game from their own disc image (Redump, Guitar Hero (USA) v1.00). The resulting exe holds code translated from your disc: **it is for you only; do not distribute it.** Unofficial; not affiliated with Harmonix, RedOctane or Activision.

**Building the Windows exe** (Ubuntu 24.04, or WSL on Windows; about 20 minutes the first time):

```sh
sudo apt install -y git cmake ninja-build python3 build-essential pkg-config curl clang-20 clang-tools-20 lld-20 llvm-20
curl -LO https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
tar xf xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
./xwin-0.6.5-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --use-winsysroot-style --output ~/xwin-sdk
git clone https://github.com/Diegots123/GH1Recomp-GHDS.git && cd GH1Recomp-GHDS
python3 tools/gh1.py verify "Guitar Hero (USA).iso"
python3 tools/gh1.py build --windows ~/xwin-sdk "Guitar Hero (USA).iso"
```

This makes `build/GH1Recomp-win64.zip`: extract it, put the ISO in its `iso` folder and run `GH1Recomp.exe` (Windows 10/11 64-bit, OpenGL 4.3). For a Linux build, install the X11/OpenGL/ALSA/PulseAudio/D-Bus/udev/FFmpeg development packages listed above and run `tools/gh1.py build` without `--windows`.

**Credits:** [GH Classic](https://github.com/noahbaxter/GHClassic) (noahbaxter), which this project is built on; [PS2Recomp](https://github.com/ran-j/PS2Recomp); [PCSX2](https://github.com/PCSX2/pcsx2) (GS and microVU, unmodified); SDL3, SDL_GameControllerDB, raylib, FFmpeg. Licensed under the GPL-3.0.

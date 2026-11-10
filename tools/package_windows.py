#!/usr/bin/env python3
"""Packages the Windows build: GH1Recomp.exe, the DLLs it loads, settings,
the readme and every licence, zipped.

    tools/package_windows.py <build dir> <output .zip>

The exe holds code recompiled from the player's own disc: the package is
for that player. Nothing from the disc itself goes in (the exe reads the
disc image the player supplies)."""

import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJDUMP = shutil.which("llvm-objdump-20") or shutil.which("llvm-objdump")


def imports(binary):
    out = subprocess.run([OBJDUMP, "-p", str(binary)], capture_output=True, text=True, check=True).stdout
    return [line.split()[-1] for line in out.splitlines() if "DLL Name" in line]


def needed_dlls(exe, search):
    """The DLLs beside the build's exe that it loads, directly or through another."""
    local = {p.name.lower(): p for p in search.glob("*.dll")}
    need, todo = {}, [exe]
    while todo:
        for name in imports(todo.pop()):
            path = local.get(name.lower())
            if path and path.name not in need:
                need[path.name] = path
                todo.append(path)
    return sorted(need.values())


def cache_path(build, key):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(key + ":"):
            return Path(line.split("=", 1)[1])
    sys.exit(f"{key} is not set in {build}")


def licence_files(build):
    pcsx2 = cache_path(build, "GH1_PCSX2_DIR")
    sdl3 = cache_path(build, "GH1_SDL3_DIR")
    return [
        ("GH1 Recomp", ROOT / "LICENSE"),
        ("PS2Recomp (runtime)", ROOT / "lib" / "PS2Recomp" / "LICENSE"),
        ("PCSX2: GS, microVU and common code (GPL-3.0-or-later)", pcsx2 / "COPYING.GPLv3"),
        ("SDL3", sdl3 / "LICENSE.txt"),
        ("raylib", build / "_deps" / "raylib-src" / "LICENSE"),
        ("Dear ImGui (in PCSX2's GS)", pcsx2 / "3rdparty" / "imgui" / "LICENSE.txt"),
        ("{fmt} (in PCSX2's GS)", pcsx2 / "3rdparty" / "fmt" / "LICENSE"),
        ("cpuinfo (in PCSX2's GS)", pcsx2 / "3rdparty" / "cpuinfo" / "LICENSE"),
        ("fast_float (in PCSX2's GS)", pcsx2 / "3rdparty" / "fast_float" / "LICENSE-MIT"),
        ("SDL_GameControllerDB (gamecontrollerdb.txt)", ROOT / "dist" / "windows" / "gamecontrollerdb-LICENSE.txt"),
    ]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build, out_zip = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    runner = build / "ps2xRuntime"
    exe = runner / "ps2EntryRunner.exe"
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "GH1Recomp-win64"
        (stage / "iso").mkdir(parents=True)
        shutil.copy2(exe, stage / "GH1Recomp.exe")
        for dll in needed_dlls(exe, runner):
            shutil.copy2(dll, stage / dll.name)
        for name in ("GH1Recomp.ini", "LEIA-ME.txt", "gamecontrollerdb.txt"):
            shutil.copy2(ROOT / "dist" / "windows" / name, stage / name)
        # The list of shader programs GH1 draws with, made by playing it: a
        # first run then prepares them all as it opens. Not in the public
        # repository (it is made from the game); each player's own runs
        # write theirs to cache\ anyway.
        programs = ROOT / "dist" / "windows" / "gh1_programs.bin"
        if programs.is_file():
            shutil.copy2(programs, stage / programs.name)
        # Windows line ends in the texts meant to be opened, however git checked them out.
        for name in ("GH1Recomp.ini", "LEIA-ME.txt"):
            text = stage / name
            text.write_bytes(text.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        (stage / "iso" / "COLOQUE-A-ISO-AQUI.txt").write_text(
            "Coloque aqui o .iso do seu Guitar Hero (USA), SLUS-21224 v1.00.\r\n"
            "Put your Guitar Hero (USA) disc image (SLUS-21224 v1.00) here.\r\n")

        text = []
        for title, path in licence_files(build):
            text.append("=" * 78 + "\n" + title + "\n" + "=" * 78 + "\n\n" + path.read_text(errors="replace") + "\n")
        ffmpeg = build / "ThirdParty" / "ffmpeg-prefix" / "src" / "ffmpeg_external"
        for lic in sorted((ffmpeg / "licenses").glob("*")) if (ffmpeg / "licenses").is_dir() else []:
            if lic.is_file():
                text.append("=" * 78 + "\nFFmpeg DLLs: " + lic.name + "\n" + "=" * 78 + "\n\n" +
                            lic.read_text(errors="replace") + "\n")
        (stage / "LICENCAS.txt").write_text("\n".join(text).replace("\r\n", "\n").replace("\n", "\r\n"))

        out_zip.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(stage.rglob("*")):
                z.write(path, path.relative_to(stage.parent))
    print(out_zip, out_zip.stat().st_size)


if __name__ == "__main__":
    main()

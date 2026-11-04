# Development setup

Everything runs in the Claude desktop app's Linux environment on the Mac (Ubuntu 22.04, arm64), inside the project folder. Nothing under `toolchain/`, `modwerk/`, `digiemu/` or the OS file is committed (see `.gitignore`).

## Folder layout

| Path | What | In git |
| --- | --- | --- |
| `Digitakt_OS1.54.syx` | Your own official OS download | never |
| `toolchain/src/` | binutils 2.47 and GCC 16.2.0 sources, checksum-verified | no |
| `toolchain/m68k-elf/` | the built ColdFire toolchain | no |
| `toolchain/node-v24.22.2-linux-arm64/` | Node 24 from nodejs.org, checksum-verified | no |
| `modwerk/` | shallow clone of repeat98/modwerk, build environment only | no |
| `digiemu/` | clone of irpina/digiemu (Modwerk pins c1b5735) | no |
| `env.sh` | `source env.sh` puts Node 24 and m68k-elf first on PATH | no |

## Network

Claude settings → Capabilities → Create and edit files → **Use the internet while making files** on, with `ftp.gnu.org`, `gcc.gnu.org` and `nodejs.org` added on claude.ai. npm, PyPI and GitHub are allowed by default.

## Toolchain

The same binutils and GCC releases and checksums as Modwerk's `sdk/build/Dockerfile`:

1. Download `binutils-2.47.tar.xz` from ftp.gnu.org and `gcc-16.2.0.tar.xz` from gcc.gnu.org into `toolchain/src/`, checking the SHA-256 and SHA-512 the Dockerfile pins.
2. Run `scripts/build-toolchain.sh`. Background processes do not survive between shell calls here, so run its steps in calls under three minutes; `make` resumes where it stopped.
3. Check: `m68k-elf-gcc --version` reports GCC 16.2.0.

## Node 24

From `nodejs.org/dist/latest-v24.x/`, checked against `SHASUMS256.txt`. The folder does not allow symbolic links, so `bin/npm`, `bin/npx` and `bin/corepack` are small shell launchers instead of the tarball's links.

## Emulator (digiemu)

1. `git clone https://github.com/irpina/digiemu digiemu && git -C digiemu checkout c1b5735` (Modwerk's pinned commit).
2. `uv` from PyPI (`pip install --target toolchain/uv-pkg uv`); `env.sh` keeps uv's Python 3.12 and cache inside `toolchain/`. Then `uv sync --frozen` in `digiemu/`.
3. The patched Unicorn: clone Unicorn tag 2.1.4 (commit 8028ec4) to `toolchain/unicorn-src`, check and apply the six patches in `digiemu/patches/` by their SHA-256 (as `digiemu/tools/install-patched-unicorn.sh` does), build with cmake and ninja from PyPI (`-DUNICORN_ARCH=m68k`), copy `libunicorn.so.2` into the venv's `unicorn/lib/`. `python -m emu.unicorn_compat` must report `"compatible": true`.
4. `python -m emu.portable --add ../Digitakt_OS1.54.syx --yes` sets up OS 1.54 (SHA-256 f78ba80f…3cf6, the release FX Track and CC Recall were tested on) and boots it to a settled main-screen snapshot.
5. `toolchain/emu-dt154.env` holds the `DT2_*` paths for that firmware folder. `scripts/dtshot.py` runs digiemu's headless panel driver with the app's emulator flags and saves the screen as PNG. digiemu's control-name table is mapped on OS 1.53, so key names do not resolve on 1.54 yet.

## Modwerk

`git clone --depth 1 https://github.com/repeat98/modwerk modwerk`, then `npm ci --ignore-scripts` and `npm run modules:check`, which passed on 4 November 2026 (commit ced6490).

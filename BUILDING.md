# Building and testing

Everything below was set up on Linux (Ubuntu 22.04, arm64) inside the repository folder; macOS works the same way with the matching downloads. Nothing under `toolchain/`, `modwerk/`, `digiemu/` or the OS file is committed (see `.gitignore`).

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

The downloads come from `ftp.gnu.org`, `gcc.gnu.org`, `nodejs.org`, npm, PyPI and GitHub.

## Toolchain

The same binutils and GCC releases and checksums as Modwerk's `sdk/build/Dockerfile`:

1. Download `binutils-2.47.tar.xz` from ftp.gnu.org and `gcc-16.2.0.tar.xz` from gcc.gnu.org into `toolchain/src/`, checking the SHA-256 and SHA-512 the Dockerfile pins.
2. Run `scripts/build-toolchain.sh` (`make` resumes where it stopped if interrupted).
3. Check: `m68k-elf-gcc --version` reports GCC 16.2.0.

## Node 24

From `nodejs.org/dist/latest-v24.x/`, checked against `SHASUMS256.txt`. If your folder does not allow symbolic links, make `bin/npm`, `bin/npx` and `bin/corepack` small shell launchers instead of the tarball's links.

## Emulator (digiemu)

1. `git clone https://github.com/irpina/digiemu digiemu && git -C digiemu checkout c1b5735` (Modwerk's pinned commit).
2. `uv` from PyPI (`pip install --target toolchain/uv-pkg uv`); `env.sh` keeps uv's Python 3.12 and cache inside `toolchain/`. Then `uv sync --frozen` in `digiemu/`.
3. The patched Unicorn: clone Unicorn tag 2.1.4 (commit 8028ec4) to `toolchain/unicorn-src`, check and apply the six patches in `digiemu/patches/` by their SHA-256 (as `digiemu/tools/install-patched-unicorn.sh` does), build with cmake and ninja from PyPI (`-DUNICORN_ARCH=m68k`), copy `libunicorn.so.2` into the venv's `unicorn/lib/`. `python -m emu.unicorn_compat` must report `"compatible": true`.
4. `python -m emu.portable --add ../Digitakt_OS1.54.syx --yes` sets up OS 1.54 (SHA-256 f78ba80f…3cf6, the release FX Track and CC Recall were tested on) and boots it to a settled main-screen snapshot.
5. `scripts/emu-image.sh NAME` builds the mod into an image, adds it to digiemu and writes `toolchain/emu-NAME.env` with its `DT2_*` paths; the emulator scripts read them. `scripts/dtsession.py` drives the emulated panel from a step script (`tap GLOBAL; wait 300; snap settings`) through digiemu's `Session`, with the measured mk1 key codes, and saves PNG captures. (digiemu's `tools/dtdrive.py` uses the firmware's panel-test name table, whose order is not the runtime key map on the mk1, so it presses the wrong keys.)

## Modwerk

`git clone --depth 1 https://github.com/repeat98/modwerk modwerk`, then `npm ci --ignore-scripts` and `npm run modules:check`.

## Python for the hardware tests

`python3 -m venv ~/.venvs/dt && ~/.venvs/dt/bin/pip install python-rtmidi`, then run `scripts/hwping.py` and `scripts/hwstress.py` with that Python.

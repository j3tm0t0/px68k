#!/bin/sh
# Run the SDL (host) build headless and deterministic: screen hashes per 50 frames.
#
#   tools/c68ktest/host.sh <commit|.> <frames> [disk image]	(default roms/gradius.hdm)
#
# Builds the commit (or the working tree for ".") for i386 Linux in docker
# with hostpatch.py applied, boots the disk with roms/IPLROM.DAT and
# roms/cgrom.dat, runs <frames> frames flat out (every frame drawn, fixed
# RTC, no sound) and prints "frame N <hash of the screen> WxH" to stdout.
# Two versions that emulate the same give the same lines (about 10 minutes
# for 4000 frames of Gradius).
set -e
cd "$(dirname "$0")/../.."
IMG=px68k-i386
docker image inspect $IMG >/dev/null 2>&1 || docker build --platform linux/386 -t $IMG - <<'EOF'
FROM i386/debian:bookworm
RUN apt-get update && apt-get install -y gcc g++ make libsdl1.2-dev libsdl-gfx1.2-dev && rm -rf /var/lib/apt/lists/*
EOF
W=$(mktemp -d "${TMPDIR:-/tmp}/c68khost.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir "$W/src" "$W/keropi"
if [ "$1" = . ]; then
	tar cf - --exclude=roms --exclude='*.o' --exclude='*.PBP' --exclude='*.elf' --exclude='*.prx' . | tar xf - -C "$W/src"
else
	git archive "$1" | tar xf - -C "$W/src"
fi
python3 tools/c68ktest/hostpatch.py "$W/src" >/dev/null
cp roms/IPLROM.DAT "$W/keropi/iplrom.dat"
cp roms/cgrom.dat "$W/keropi/"
cp "${3:-roms/gradius.hdm}" "$W/disk.img"
docker run --rm --platform linux/386 -v "$W":/w $IMG sh -c "
	cd /w/src && make -j8 MOPT= CDEBUGFLAGS='-O2 -DNO_MERCURY -DUSE_SDLGFX -DPX68K_VERSION=0' >/w/build.log 2>&1 &&
	mkdir -p /tmp/home/.keropi && cp /w/keropi/* /tmp/home/.keropi/ && cd /tmp &&
	HOME=/tmp/home SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOSTRUN_FRAMES=$2 /w/src/px68k /w/disk.img 2>&1 >/dev/null | grep '^frame'" ||
	{ tail -20 "$W/build.log"; exit 1; }

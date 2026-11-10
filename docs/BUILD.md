# Building from source

`package.sh` builds everything and lays out the SD card content:

```sh
MIBHMI=<MIBHMI.jar converted from your unit's MIBHMI.jxe> JAVAC=<JDK 8 javac> sh package.sh
```

It runs `hmi/build.sh` and `native/build.sh`, copies `toolbox/custom/` to `build/sd/custom/`,
adds the jar and the libraries named in `toolbox/custom/mst2-cluster-nav/preload.list` to
`build/sd/custom/mst2-cluster-nav/`, and zips the result as `build/mst2-cluster-nav-sd.zip`.
`build/` is not part of the repository; the ready-built content for P0478T is in
[release/](../release/README.md).

## Toolchain

| Part | Needs | Notes |
|---|---|---|
| `native/` (three `LD_PRELOAD` libraries for QNX 6.5 armle-v7) | **clang** and **ld.lld** | Any host. No QNX SDK and no files from the unit: the unit's `libc.so.3` and `libsocket.so.3` are replaced at link time by stubs (`native/stubs/`) that only carry the symbol names. macOS: `brew install llvm lld`; Linux: the distribution's `clang` and `lld` packages or an LLVM release tarball. |
| `hmi/` (the HMI bundle) | **JDK 8** and **`MIBHMI.jar`** | The HMI runs on IBM J9 with class files of version 46 to 49, so the bundle is compiled with `-source 1.3 -target 1.3`; JDK 8 is the newest javac that still accepts that. |
| `package.sh` | `zip` | |

Environment variables, all optional except `MIBHMI`:

| Variable | Used by | Default |
|---|---|---|
| `MIBHMI` | `hmi/build.sh`, `hmi/check.sh` | none; required |
| `JAVAC`, `JAR` | `hmi/build.sh` | `javac` on the `PATH`, `jar` next to it |
| `JDK` | `hmi/check.sh` | a macOS Zulu 8 path; set it to your JDK 8 home |
| `LLVM`, `CLANG`, `LLD` | `native/build.sh` | Homebrew's llvm/lld; set `CLANG` and `LLD` to the binaries, or `LLVM` to their directory |
| `FFMPEG`, `FFPROBE` | `native/test/aa_sps_check.sh` | on the `PATH`; the check is skipped without them |
| `MCN_AA_REPLAY` | `native/check.sh` | `replays/aa-cluster-800x480.h264`; the end-to-end decode check is skipped without a recording |

Example (Linux, an LLVM release unpacked to `$LLVM`, OpenJDK 8):

```sh
export JDK=/usr/lib/jvm/java-8-openjdk-amd64
export CLANG=$LLVM/bin/clang LLD=$LLVM/bin/ld.lld JAVAC=$JDK/bin/javac
export MIBHMI=/path/to/your/MIBHMI.jar
sh package.sh && sh native/check.sh && sh hmi/check.sh && sh toolbox/check.sh
```

## MIBHMI.jar: the unit's own HMI

The bundle calls stock HMI classes (the ASL datapool, the BAP NavSD API, `APIFactory`), so it
must be compiled against the HMI of **your own unit**. The HMI is proprietary and is **not**
part of this repository or of the package; the HMI version is what the installer checks
(`H29.319.93` for P0478T, `H29.319.120` for P0480T), because the datapool ids differ between
builds.

How to get it:

1. Make a dump of your unit with the MIB STD2 Toolbox (its dump functions copy the unit's file
   systems to the SD card; see the Toolbox's documentation).
2. In the dump, the HMI's class archive is `MIBHMI.jxe` in the HMI directory (the stock
   `runHMI.sh` names the boot class path it belongs to).
3. Convert the `.jxe` (a J9 class archive) to an ordinary `.jar` with the class files inside.
   A converter is not part of this repository; the open-source `jxe2jar` tools
   (https://github.com/luka-dev/jxe2jar, https://github.com/moradek/jxe2jar) do this.
4. Pass the jar as `MIBHMI`. Nothing from it is copied into the build: the jar is only on the
   compile class path.

`docs/LANES.md` names the SHA-256 of the P0480T `MIBHMI.jar` the original project's evidence
was taken from, if you want to compare.

## Host checks

```sh
sh native/check.sh    # protocol decoders, the real spawn/iAP2/hook logic, the Android Auto
                      # wire formats, the Android Auto hooks and the decoder module (32-bit)
sh hmi/check.sh       # the bundle's Java logic (maneuver table, PhoneLink parsing, lanes)
sh toolbox/check.sh   # the runHMI.sh guard, the installer's conflict check, the menu scripts
```

`native/check.sh` needs `python3` and a native `clang`. The Android Auto hook and decoder
checks are built 32-bit like the unit and need the host's 32-bit C library (Linux:
`gcc-multilib`), otherwise they are skipped with a message. The SPS cropping check needs
`ffmpeg`/`ffprobe`, otherwise it is skipped. The end-to-end decoder check needs a recording of
a phone's 800x480 cluster stream (`replays/aa-cluster-800x480.h264` or `MCN_AA_REPLAY`);
none is in the repository, because such a recording shows the phone owner's location. The mod
records one from your own phone when a card with the empty file `mst2-cluster-nav-capture-on`
is in the unit (`native/src/sal/capture.c`). Without the file that part is skipped.

`hmi/check.sh` must run after `native/check.sh`: the lanes check reads the snapshot the native
check writes to `native/build/check/lanes-message.txt`.

What the host checks prove and what they do not (the interposition itself, the early driver
load and the Screen/mm-renderer behaviour on QNX can only be seen on a unit) is described in
[ARCHITECTURE.md](ARCHITECTURE.md) and [ANDROID-AUTO.md](ANDROID-AUTO.md).

## Reproducibility

The native libraries are built with `-Os`, no debug information and a fixed link order, and
come out byte for byte the same on every build with the same clang/lld release (the
`release/SHA256SUMS` values for `libmcn_sal.so`, `libmcn_dm.so` and `libmcn_ipod.so` are what
the checked-in sources produce). The jar is **not** byte-reproducible: `jar` stores the time
stamps of the class files, so each build gives a different hash with identical class content;
compare the classes, not the jar.

## Layout

```
native/   C sources of the three libraries (src/sal, src/dm, src/ipod, src/common), headers,
          link stubs, build.sh, check.sh and the host tests in test/
hmi/      the Java bundle (src/mst2/clusternav), build.sh, check.sh and the host tests in test/
toolbox/  custom/ is what goes to the card (the release GEM screen, the scripts, the blocks the
          installer adds to the unit's files); expert/ holds the full GEM screen; check.sh
replays/  synthetic PhoneLink and lane scenarios for the host checks (replays/yandex-route.txt,
          replays/lanes.txt)
release/  the built SD card content for P0478T, the zip and the checksums
docs/     this documentation
```

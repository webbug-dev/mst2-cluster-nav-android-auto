# Release: ready-to-install SD card content

Built from the sources in this repository against firmware **`MST2_EU_SK_ZR_P0478T`**
(HMI `H29.319.93`, Škoda Amundsen MIB2 Standard) and verified in a car on 2026-11-10 with a
Samsung Galaxy S23 over wired Android Auto. The installer also accepts `MST2_EU_SK_ZR_P0480T`
(HMI `H29.319.120`), the original project's reference firmware; its Android Auto path was not
run in a car.

| File | What |
|---|---|
| `sd/custom/` | The SD card content: copy this `custom/` folder to the root of the MIB STD2 Toolbox card, merging it with the Toolbox's own `custom/`. |
| `mst2-cluster-nav-sd.zip` | The same content zipped (`sd/custom/...`). |
| `SHA256SUMS` | Checksums of every file above; check with `sha256sum -c SHA256SUMS` in this directory. |

`sd/custom/greenmenu/` holds the GEM screen (three items: install for the next boot, enable
permanently, uninstall) and its scripts; `sd/custom/mst2-cluster-nav/` holds the three native
libraries, the HMI bundle jar and the blocks the installer adds to the unit's files. No vendor
file is included; the jar was compiled against the unit's own HMI, which is not distributed.

Then follow **[docs/INSTALL.md](../docs/INSTALL.md)**: import the GEM screens on the unit,
Install for the next boot, reboot, test with the phone, Enable permanently.

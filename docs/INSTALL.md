# Installation guide

This guide installs the Android Auto cluster map on a MIB2 Standard head unit with the MIB STD2
Toolbox. Read it once to the end before you start; the whole procedure takes two reboots of the
unit.

## 1. Prerequisites

**Head unit.** A MIB2 Standard **navigation** unit (e.g. Škoda Amundsen) with the Active Info
Display / Virtual Cockpit. Radio units without navigation (e.g. Bolero) are not supported: they
have no SAL process and draw no cluster map; the installer refuses them.

**Firmware.** The installer accepts two HMI builds and refuses every other one:

| Firmware train | HMI version | State |
|---|---|---|
| `MST2_EU_SK_ZR_P0478T` | `H29.319.93` | Android Auto verified in a car (2026-11-10) |
| `MST2_EU_SK_ZR_P0480T` | `H29.319.120` | the original project's CarPlay reference; its Android Auto path was not run in a car |

The firmware version is shown in the unit's system information and in GEM. On another build the
install stops with `Aborted: HMI version ... is not verified for this mod`.

**Coding.** Android Auto (and CarPlay) coded on in the unit. For the phone's map on the cluster,
the cluster map over MOST must be coded on, as for the built-in navigation's map (the cluster
shows a map in its navigation view). The native arrows work without the map coding.

**MIB STD2 Toolbox.** Installed on the unit, with its Green Engineering Menu (GEM) reachable:
<https://github.com/olli991/mib-std2-pq-zr-toolbox>. Follow the Toolbox's own instructions
for that. The Toolbox's **dual navigation** patch (`NavActiveIgnore.jar`) does not mix with
this mod: the installer refuses it (`Customization -> Navigation -> Disable dual navigation`,
reboot, then install), and if it is switched on later, the mod switches itself off.

**SD card.** The release zip `mst2-cluster-nav-android-auto-sd-v1.0.zip` is a complete card (the Toolbox plus the mod, see the README's How to use). Otherwise: the Toolbox card (FAT32 or exFAT) with the Toolbox's own `custom/` folder on it. The mod's
files go next to them; the installer looks for a card that has `custom/mst2-cluster-nav/`.

**Phone.** An Android phone with Android Auto and Google Maps or Waze, connected with a USB
cable. Wireless Android Auto was not tested.

## 2. Files to copy to the card

Take `release/sd/custom/` from this repository, or unpack `release/mst2-cluster-nav-sd.zip`
(it contains `sd/custom/...`). `release/SHA256SUMS` lists the checksums. Copy the `custom/`
folder to the **root of the Toolbox card**, merging it with the Toolbox's own `custom/`. The
card then has:

```
custom/
  greenmenu/
    mst2-cluster-nav.esd            the GEM screen (three items)
    scripts/mcn_*.sh                the install, enable, uninstall and settings scripts
    ...                             the Toolbox's own screens and scripts stay
  mst2-cluster-nav/
    libmcn_sal.so                   preloaded into SAL (Android Auto / CarPlay hooks, decode mode)
    libmcn_dm.so                    preloaded into DisplayManager (CarPlay pass-through path)
    libmcn_ipod.so                  handed to mm-ipod by SAL (CarPlay route guidance)
    mst2-cluster-nav.jar            the HMI bundle (arrows, lanes, the cluster notice)
    preload.list, runHMI.block, bundles.block   what the installer adds to the three files
```

If you built the package yourself, `build/sd/custom/` is the same layout (see
[BUILD.md](BUILD.md)).

## 3. Import the GEM screen and scripts

The GEM screens and scripts do **not** run from the card: the Toolbox copies them to the unit,
and the unit keeps that copy until it is imported again.

In the car: **GEM → MIB STD2 Toolbox → Customization → GreenMenu → Copy custom Green
Engineering Menu screens and scripts to unit.** Then leave GEM and open it again: the entry
**Cluster_Navigation** appears under Customization.

Repeat this step every time the menu or the scripts on the card change (an update of the mod,
switching to the expert menu) and after every Toolbox update, which replaces the custom
screens. A real "nothing changed after the update" report had exactly this step missing: the
unit was still running the old scripts.

## 4. Install for one boot (trial)

**GEM → Customization → Cluster_Navigation → Install / update: the NEXT boot only (trial).**

The script checks the HMI version, the unit type and the dual navigation patch, copies the
libraries and the jar to the unit (each copy compared byte for byte before it takes its name),
and, on a first install, backs up the three stock files it is going to touch:

- on the unit, in `/tsd/var/mst2-cluster-nav/backup/`, and
- on the card, in **`mst2-cluster-nav-backup/`** (`runHMI.sh`, `bundles.properties`,
  `cpu.conf`). **Keep this folder.** Nothing is changed unless all three backups exist.

Then it adds one block to each of the three files (atomically: written in full next to the
file, then renamed over it) and arms **one trial boot**. The screen ends with:

```
Installed. The NEXT boot loads the mod once (trial).
If the unit works, choose 'Enable permanently'. Emergency: an SD card
with an empty file 'mst2-cluster-nav-disable' stops the mod before the HMI starts.
```

**Reboot the unit.** On this boot the mod loads once. If the unit is reset again before the
HMI has settled (about a minute), the trial is over and the unit comes up stock.

## 5. Test with the phone

1. Connect the phone by USB and let Android Auto start on the centre screen.
2. Switch the cluster to its **navigation (map) view**.
3. Start a route in Google Maps (or Waze). Within a few seconds the phone's map replaces the
   built-in map in the cluster's view, and the cluster's own arrow, distance and street line
   follow the phone's route.
4. Unplug the phone: the cluster goes back to the built-in map on its own.

Without a phone the cluster shows the built-in map or arrows exactly as stock.

If nothing is installed at all on the next boot, that is the trial working as designed: the
mod loads only once until you enable it.

## 6. Enable permanently

Satisfied with the trial? **GEM → Customization → Cluster_Navigation → Enable permanently:
every boot.** From the next boot the mod loads every time. (If the mod took itself out in the
meantime, the script says so and names the reason; run Install again.)

## 7. Updating

1. Copy the new `custom/` from the release over the card's `custom/`.
2. Import the GEM screens and scripts again (step 3) if the menu or scripts changed.
3. **Install / update: the NEXT boot only** → reboot. An update replaces the mod's parts
   instead of adding them twice, and it resets the expert menu's test modes (test video, demo
   guidance, hidden built-in map) and the switch files of earlier builds.
4. **Install clears the permanent flag.** After the test boot press **Enable permanently**
   again, otherwise the mod is gone from the boot after.

## 8. Uninstall

**GEM → Customization → Cluster_Navigation → Uninstall: stock files back, then reboot.**

The script removes the mod's block from the three files (the stock content around it is left
as it was; the result is byte for byte the stock file), removes the libraries from
`/tsd/lib/mst2-cluster-nav` once `cpu.conf` no longer names them, and deletes
`/tsd/var/mst2-cluster-nav`. Reboot afterwards. If any trace could not be removed, the script
says so and keeps the backups on the unit.

**Uninstall the mod before uninstalling the Toolbox**: removing the Toolbox removes the menu
scripts, and with them the mod's Uninstall button.

## 9. The expert menu

The release menu has three items and every setting at its default. The full menu with the
settings and the test modes is `toolbox/expert/greenmenu/mst2-cluster-nav.esd` in the
repository. To use it, copy that file over `custom/greenmenu/mst2-cluster-nav.esd` on the card
and import the screens again (step 3). The scripts it calls are already on the card.

| Item | Effect |
|---|---|
| Install / update, Enable permanently, Arm another trial boot, Disable | As in the release menu, plus one more trial boot and a plain off switch (off from the next boot; the files stay). |
| Phone map and native arrows (default) / Native arrows only | Offer the cluster display to the phone, or keep the native arrows only. |
| Android Auto on the cluster: on (default) / off | Off leaves Android Auto completely stock. |
| Android Auto map: 1280x720 cropped / 800x480 stretched | Geometry of the **pass-through** stream only; no effect in decode mode. |
| Android Auto map: key frames by a focus cycle (default) / no focus cycle | How a key frame is asked for after a lost unit. |
| Android Auto map: decoded on the unit (default) / passed through | Decode mode is the only path that shows a picture on this firmware; "passed through" is the old experimental path and gives a **black cluster** on P0478T. Switching back to "decoded" also takes "Built-in map: hidden" back to "kept". |
| Built-in map: kept (default) / hidden | Pass-through only. In decode mode the built-in map must stay underneath: DisplayManager captures the cluster display only while that map redraws, and on this firmware hiding it stopped the cluster stream. |
| Demo guidance on / off | Synthetic arrows in a parked car, no phone needed (after a reboot). Checks the HMI part of the install. |
| Test video on / off | Plays `mst2-cluster-nav-video.h264` from the card on the cluster without a phone (after a reboot), see troubleshooting. |
| Status | HMI version, registered or not, the last reason the mod switched itself off, every setting. |
| Save diagnostics to the SD card | Writes `mst2-cluster-nav-diag.txt` (the mod's log and the unit's state) and `mst2-cluster-nav-iap2.log` to the card. Run it with the phone connected. |
| Uninstall | As in the release menu. |

Changes to the Android Auto settings apply from the next phone connection, the test modes
after a reboot, as each script says. **Install / update** turns the test modes off again.

## 10. Troubleshooting

**Cluster_Navigation is not in the menu, or the menu looks old.** Step 3 was skipped or ran
before the files were on the card. Check that the card has
`custom/greenmenu/mst2-cluster-nav.esd` and `custom/greenmenu/scripts/mcn_install.sh`, import
again, leave and reopen GEM.

**`Aborted: HMI version ... is not verified`.** Your firmware is not one of the two builds
above. Do not force it; the native parts would stay stock anyway.

**`Aborted: no SD card with custom/mst2-cluster-nav`.** The folder is not in the card's root
`custom/`, or the card is not mounted. Check the layout in section 2.

**`Aborted: the toolbox 'dual navigation' patch ... is installed`.** Disable it in the
Toolbox's Navigation menu, reboot, install again.

**The cluster shows the built-in map, not the phone's.**

- Is the cluster in its navigation view, is Android Auto running on the centre screen, is a
  route active in Google Maps?
- Is the cluster map over MOST coded on? Without it the cluster has no map view to replace.
- Was this the trial boot, and has the unit booted again since? Then the mod is off by design:
  Install again, or Enable permanently.
- Expert menu → Status: `registered: yes`, `permanent: yes` (or `trial armed`), `android
  auto: on, decoded on the unit`, `built-in map: kept`. A line `turned itself off: ...` names
  the reason (kill-switch card present, an HMI restart before the mod settled, the dual
  navigation patch, a missing library); fix the cause and Install again. `... hooks off after
  repeated crashes` means a native part crashed twice in a row; Install turns it on again.

**The cluster's map view is black while the phone is connected.** Expert menu → Status: if it
says `passed through`, switch to **decoded on the unit**; pass-through leaves this firmware's
cluster black. If it says `built-in map: hidden`, choose **Built-in map: kept**. Both settings
are release defaults; Install / update restores them.

**The picture pauses or breaks up.** The phone sends a key frame only on request; the mod
asks for one after a lost unit. Switching the cluster to another view and back, or
reconnecting the phone, starts a fresh stream.

**Checking the video path without a phone.** Put a recording of a phone's 800x480 cluster
stream on the card as `mst2-cluster-nav-video.h264` (H.264 Annex-B, SPS and PPS in front of the
key frames), expert menu → **Test video on**, reboot, cluster in the map view: after about
20 s the recording plays on the cluster in a loop. Such a recording is made by the mod itself:
a card with the empty file `mst2-cluster-nav-capture-on` in its root records the next
Android Auto cluster stream to `mst2-cluster-nav-capture.h264` on that card (no recording ships
with the repository, because it shows the phone owner's location). **Test video off** and a
reboot end the test; Install / update turns it off too.

**Taking a diagnostic file.** Expert menu → **Save diagnostics to the SD card** with the phone
connected and the cluster in the map view. The card then has `mst2-cluster-nav-diag.txt`
(the mod's log `/dev/shmem/mst2-cluster-nav.log`, the unit's network and display state, the
settings) and `mst2-cluster-nav-iap2.log`. What the log lines mean is explained in
[ANDROID-AUTO.md](ANDROID-AUTO.md), "What the log shows" and "Checking an installation".

**Emergency stop.** Insert a card with an empty file `mst2-cluster-nav-disable` in its root
(any card, the Toolbox card included). The guard in `runHMI.sh` keeps the mod out before the
HMI starts, and the native hooks turn to pass-through within two seconds. Keep such a card in
the glovebox.

**After a Toolbox update** the custom menus are gone: import them again (step 3). The mod keeps
running in the meantime.

## 11. What the mod changes on the unit, and what it does not

- **No firmware file is replaced.** The install adds one marked block to each of three text
  files: `/tsd/hmi/runHMI.sh` (the guard and the mod's jar appended to the boot class path),
  `/tsd/hmi/HMI/config/bundles.properties` (the HMI bundle, id 58, started late after "GUI
  ready") and `/tsd/etc/system/cpu.conf` (`LD_PRELOAD` of `libmcn_sal.so` for SAL and
  `libmcn_dm.so` for DisplayManager). Uninstall removes the blocks; the result is the stock file
  byte for byte.
- **Own directories only:** `/tsd/lib/mst2-cluster-nav/` (the libraries, on the system
  partition next to `cpu.conf`, so that a service whose preload is missing never fails to
  start) and `/tsd/var/mst2-cluster-nav/` (the jar, the settings, the backups). RAM marks under
  `/dev/shmem/`.
- **Stock copies** of the three files are kept on the unit and on the card
  (`mst2-cluster-nav-backup/`) before the first change. Keep the card copy: it is the way back
  if the unit's HMI ever stopped starting.
- **The firmware's own paths only.** Guidance data enters where the built-in navigation puts it
  (the HMI datapool, then BAP NavSD / MOST NavInfo); video enters the picture DisplayManager
  already encodes. Nothing the cluster sees is invented.
- **Fail to stock.** Each part checks the firmware build it hooks and passes every call through
  on an unknown one; a crashed part switches itself off after two crashes in a row; the guard
  in `runHMI.sh` can only ever switch the mod off; with no phone stream the cluster shows the
  stock map within 1.5 s.
- **Not touched:** the coding and adaptation of the unit or the cluster, the CAN/MOST side, the
  built-in navigation, Bluetooth, the Toolbox itself, any other HMI class (the jar is appended
  to the boot class path, so no stock class is shadowed).

Modifying a head unit may void its warranty; use at your own risk.

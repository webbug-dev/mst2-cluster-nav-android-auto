# Changelog

## v1.1 — 2026-11-11

### Fixed

- **Map position.** The map on the cluster sat too low: part of it was hidden under the bottom
  bar and the top was wasted under the status line. It is now framed to the part of the
  cluster you can actually see.
- **Car marker.** Your current position (the car arrow) was off screen. It is now always
  visible and sits in the horizontal middle of the cluster, with the road ahead above it.
- **Maneuver card.** Google Maps' green card with the next turn, the distance and the arrival
  time was cut off by the status line. It is now fully visible at the right edge of the
  cluster, clear of the status line and of the bottom corner.
- The "Google Maps" logo stays hidden under the bottom bar.

### Added

- **Adjustable layout.** If the map does not sit right on your cluster, you can move it
  without rebuilding anything: put a file `mst2-cluster-nav-layout.txt` on the root of the SD
  card with five numbers (`top bottom left right shift`) and reconnect the phone. The release
  zip contains `mst2-cluster-nav-layout.example.txt` with the default values. Invalid values
  are ignored and the default layout is used.

### Updating from v1.0

Unzip the new release onto the card (replace the files), then in the Green Engineering Menu
run **Install / update** again and reboot. Only `libmcn_sal.so` has changed.

## v1.0 — 2026-11-10

First release: the Android Auto map (Google Maps, Waze) in the Virtual Cockpit's navigation
view on MIB2 Standard (verified on P0478T), together with the cluster's own turn arrows, a
three-item install menu in the Green Engineering Menu, a full uninstall and an SD-card kill
switch. The release zip is a complete SD card: MIB STD2 Toolbox v1.5.1 plus the mod.

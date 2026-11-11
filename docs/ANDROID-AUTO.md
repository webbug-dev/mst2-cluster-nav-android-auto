# Android Auto on the cluster

Status: **verified in a car on 2026-11-10** (Škoda Octavia III facelift, Amundsen MIB2 Standard
`MST2_EU_SK_ZR_P0478T`, HMI `H29.319.93`, Active Info Display, Samsung Galaxy S23 over USB,
Google Maps): the phone accepted the cluster display, its map was shown on the cluster through
the decode mode described below, and the cluster's own arrows followed the phone's route.
Everything the hook relies on either resolves at run time (and leaves Android Auto stock when it
does not) or is one of the layout facts below, which the three public receiver builds studied
share. On P0478T the exports and every layout fact were also checked by disassembling the
unit's own library (sha256 `b4fae78e…`); what that settled is marked "P0478T" below.

Reference hardware: cluster Continental AID 10.25" `5E0920790A`; the original project's
reference unit is the sister firmware `MST2_EU_SK_ZR_P0480T`.

## What it does

With an Android phone connected, Google Maps (or Waze) shows its map on the cluster and the
cluster's own arrows, lane symbols, street and distances follow the phone's route, exactly as
the CarPlay side of this mod does. The same GEM switches apply (cluster view, frame rate,
key-frame policy are CarPlay-only; mode "Native arrows only" applies to both). Android Auto
has its own switches: on/off, the stream geometry, and the key-frame focus cycle.

## How it works

```
Android ─AA (USB)─┬─ ServiceDiscoveryResponse ◄─ rewritten on the wire [A] (+cluster sink, input, nav)
                  ├─ cluster video sink (ours) ─► H.264 1280x720, SPS cropped to 1010x376 ─► shared memory ─► [B] DisplayManager hook
                  └─ navigation status (ours) ─► NavigationState / CurrentPosition ─► PhoneLink snapshot ─► [D] HMI bundle ─► BAP NavSD
```

| | Process | Mechanism | Job |
|---|---|---|---|
| A | `tsd.mibstd2.sal.main` | `LD_PRELOAD` (same `libmcn_sal.so`) | interposes exports of `libext.google.gal.receiver.so`: registers three endpoints of its own, rewrites the discovery response, speaks the media channel, decodes navigation |
| B | DisplayManager | unchanged | the phone's access units replace the encoder's output while the cluster shows the map (`dm.c`) |
| D | HMI | unchanged | `PhoneLink` takes the same datagrams (`video=1`, `active=1 type=..`) as for CarPlay |

Code: `native/src/sal/aa.c` (the hooks, the endpoints, the stream), `native/src/sal/aa_proto.c`
(wire formats, pure functions, host-checked by `native/test/aa_check.c` and
`native/test/aa_sps_check.sh`).

### The receiver library

SAL loads Google's receiver library (GAL) and drives it: `GalReceiver::init` on connect,
`registerService` for its endpoints, `shutdown` on disconnect. The library exports its C++
symbols, so the preload takes the calls (like `sal.c` takes the CarPlay plug-in's) and passes
them on. Every function used is resolved from the library by name at the first call; if one
of the required ones is missing, Android Auto stays stock and the log says which.

Layout facts (identical on the MST2 P0480T library, on the Harman MHI2 receiver and on the
Audi MHI2Q receiver, all three studied in public projects):

- `ProtocolEndpointBase`: +0 vptr, +4 "channel open", +5 channel, +8 router, +0xc service id;
  vtable slots 0/1 destructors, 2 `onChannelClosed(channel)`, 3 `mayOpenChannel`,
  4 `onChannelOpened(channel)`, 5 `routeMessage(channel, id, shared_ptr<IoBuffer>&)`,
  6 `handleRawMessage`, 7 `addDiscoveryInfo(response)`.
- `MessageRouter`: endpoint table at `router + (id + 0x40) * 4`; `MessageRouter` sits at
  `receiver + 4` on MST2 (learned from the first stock registration, assumed otherwise).
- `shared_ptr<IoBuffer>`: +4 → `IoBuffer { base +0, offset +8, end +12 }`; the payload starts
  with the 2-byte message id.

P0478T adds what the hooks must give back: `GalReceiver::init` returns a bool and SAL starts
Android Auto only when it is not 0; `MessageRouter::queueOutgoing` returns ChannelManager's
result, which some callers test; an endpoint's `onChannelClosed` returns whether the channel
was open, and the router frees the channel only then; an endpoint's byte at +0x10 selects
`handleRawMessage` instead of `routeMessage` (0 in ours). `MessageRouter::init` clears the
endpoint table on every `GalReceiver::init`, so registering at its end each time is required.
Stock SAL has seven `registerService` call sites, for its audio sink and source, video sink,
input, sensor, Bluetooth and vendor-extension endpoints, and none for navigation status; the
library itself asks for protocol 1.3. `native/test/aa_hook_check.c` runs the real hooks around a stand-in
library that behaves this way.

Our endpoints are static objects with a private copy of the `ProtocolEndpointBase` vtable
(slots 2, 4, 5, 6, 7 ours, destructors no-ops). They are registered at the end of
`GalReceiver::init`, which runs again on every connection, with three free service ids from
0x40 up (the stock ids are low). `shutdown` ends the stream and the guidance.

### Discovery

`addDiscoveryInfo` of our endpoints does nothing: the service discovery response is
rewritten where it leaves the router (`MessageRouter::queueOutgoing`, channel 0, id 6), as
protobuf bytes, with no object layouts involved (`aa_discovery_rewrite`):

- every stock video sink gets `display_id 0, display_type MAIN`, every stock input source
  `display_id 0` (a phone that sees a second display needs the first one named);
- a `MediaSinkService` (VIDEO, H.264, `display_id 1, display_type CLUSTER`) with one
  `VideoConfiguration`: 1280x720, the stock frame-rate enum, margins 270x344 (the phone lays
  its UI out in the centred 1010x376 viewport), density 105, `UiConfig` with the stock cluster
  safe area (sal.h `MCN_SAFE_*`, shifted by the viewport origin) and the dark theme. In
  decode mode (800x480, the default) the insets are top 83, bottom 128, left 240, right 10
  (`AA_DEC_INSET_*` in mcn_aa.h; a card file `mst2-cluster-nav-layout.txt` overrides them):
  Google Maps anchors its maneuver card to the bottom right of the content area, draws its
  logo 17 px above the area's bottom edge and centres the car in the area left of the card
  (car, 2026-11-11). These values put the card fully on the cluster at its right edge, the car
  in the horizontal middle and the logo under the cluster's bottom bar;
- an `InputSourceService` for display 1 (DPAD_CENTER only; a display without one is refused);
- a `NavigationStatusService` (interval 1000 ms, type ENUM).

### The cluster stream

The phone opens a channel for the video sink; our `routeMessage` answers the media
protocol itself: `SETUP_REQUEST` → `SETUP_RESPONSE` (OK, one unacked) and `VIDEO_FOCUS`
(projected); `START` → the stream begins (new generation in the shared memory, an IDR is
awaited); each `MEDIA_WITH_TIMESTAMP` / `MEDIA` → `ACK`, and the access unit is published;
`STOP` ends the stream; `VIDEO_FOCUS_REQUEST` is granted as asked.

Access units arrive as Annex-B. Every SPS is rewritten (`aa_sps_crop`) so the decoded picture
is cropped to the viewport: 1280x720 coded, 1010x376 shown at (134, 172); the cluster's own
decoder honours cropping (the stock stream is 1024x384 cropped to 1010x376). SPS and PPS are
kept and put in front of every IDR, as `mcn_video.h` requires. GEM "800x480 stretched" asks
the phone for 800x480 without margins or cropping instead: the cluster then stretches the
picture (fallback if the cropped stream shows nothing).

Key frames: Android Auto has no key-frame request. When DisplayManager asks for one
(`want_idr`), the sink sends `VIDEO_FOCUS` native and, after the phone's `STOP` (or 500 ms),
projected again; the phone restarts its encoder with SPS, PPS and an IDR. At most one cycle
per 2 s. GEM can switch the cycle off.

Liveness: `MessageRouter::routeMessage` runs on the receiver's thread for every incoming
message (the phone's pings keep coming while a still map sends no frames); the stream's
housekeeping (alive mark, "video=1" to the HMI, key-frame cycle) runs there and after every
frame.

### Navigation

The hook asks the phone for protocol 1.7 (`Controller::sendVersionRequest` is replaced by a
request for 1.7; a response above major 1 is told to the receiver as 1.7). The phone then
sends `NavigationState` (0x8006: steps with maneuver, road, lanes) and
`NavigationCurrentPosition` (0x8007: distance and time to the step and to the destination,
current road) about once a second instead of the 1.x pair; both generations are decoded. The
maneuver is mapped to the iAP2 type `Maneuvers.java` already draws (`aa_nav_iap2_type`),
roundabouts to exit number and angle, lanes to the `lane<n>=` rows (`Lanes.java`), and the
whole snapshot goes to `PhoneLink` as one datagram per message. `NAV_START` is sent when the
navigation channel opens. With 1.7 the stock endpoints receive messages they do not know and
would answer "unexpected message", which makes the phone drop the session; that reply is
suppressed while our endpoints are registered.

### The cluster's notice

The stock HMI sends BAP NavSD InfoStates 6 ("navigation on the mobile device") while ASL
property 2910005 says Android Auto guides: `InfoStates.getCurrentBAPInfoState` asks the
CarPlay, the Android Auto and the CarLife service in turn (HMI `H29.319.93`). The bundle's
`ClusterAndroidAuto` stands in `APIFactory`'s Android Auto slot the way `ClusterCarPlay` does
for CarPlay and answers false while the phone owns the cluster.

### Switching off

The kill switch, `mcn_active()` and the trial logic of `common/hook.c` apply unchanged: when
the mod is off, `init` registers nothing and every interposer passes through. GEM "Android
Auto: off" (`/tsd/var/mst2-cluster-nav/aa-off`) does the same for Android Auto alone. The
crash guard of the SAL component covers the stream (`mcn_busy`).

## What the log shows

The items below were the open questions before the first car run; the "P0478T" notes are what
the disassembly and the car run (2026-11-10) settled. They remain the checklist for a unit that
does not behave: read `/dev/shmem/mst2-cluster-nav.log` (expert menu: Save diagnostics) after
one connection.

1. `AA: GAL exports resolved` and the optional-exports bitmask: which exports this build has.
   P0478T: all present (bitmask 31).
2. `AA: router at receiver + 4`: the first stock registration during `init`. P0478T: so it is
   (`GalReceiver::registerService` adds 4 and jumps to the router's).
3. `AA: outgoing message 6` followed by `discovery response rewritten`: the response really
   leaves through `queueOutgoing`. If only `outgoing message` lines without a 6 appear, it goes
   another way and the rewrite must move. P0478T: `Controller::sendServiceDiscoveryResponse`
   calls `MessageRouter::queueOutgoing` through the PLT with id 6.
4. `AA: cluster video channel N` and `cluster setup, codec 3`: the phone accepted the display.
   Nothing here = the phone refused the discovery (check the input service, the frame-rate
   enum, the density). P0478T, car: accepted (Samsung Galaxy S23).
5. `AA: stream width 800` (decode mode) and `key frame`: frames arrive and parse. P0478T, car:
   the map was shown through the decode mode; the pass-through variants (`1280` cropped,
   "800x480 stretched") leave the cluster black on this firmware, see "Decode mode" below.
6. `AA: video focus sent, mode 2` followed by `cluster stream ended: stop` and a new key frame:
   the focus cycle works as a key-frame request (in decode mode it is only run after a lost
   unit).
7. `AA: navigation message 32774` (0x8006): protocol 1.7 took; `32772`/`32773` mean 1.x.
   P0478T, car: the cluster's arrows followed the route.
8. The cluster's "navigation on the mobile device" notice: the stock HMI raises it for
   Android Auto through its own service (see "The cluster's notice"); `hmi: Android Auto route
   guidance hidden from the cluster while the phone owns it` in the log says the counterpart
   is in place. If the notice still shows, the property has another reader on this cluster.

Not possible: steering-wheel zoom of the phone's cluster map (no such command in the Android
Auto protocol for the cluster display).

## Decode mode: the phone's map decoded on the unit

Status: **verified in the car (P0478T, 2026-11-10): first the decoded test recording, then the
live Google Maps map of a Samsung Galaxy S23 were shown on the cluster; the default since.**
Code: `native/src/sal/decode.c`, wiring in `aa.c` and `lab.c`, host check
`native/test/decode_check.c`. An earlier run on the same car had the picture on the centre
screen instead: a renderer window that joins our group is composed on its own display on this
Screen, so the output URL names the display index and each joined window is moved to the
cluster display.

### Why

The car (2026-11-09, P0478T) showed that the pass-through only works with the stock geometry:
a 1024x384 clip substituted in DisplayManager is shown, but the phone can only send 800x480 or
1280x720, and a change of the coded size freezes the cluster's decoder. So in decode mode the
phone's stream is not handed to the cluster at all. It is decoded on the head unit, the way
the stock SAL decodes the phone's main video (a second mm-renderer context of our own), into a
window on the cluster source display `dpy-6` (Screen display id 6, 1024x384). DisplayManager
captures that display and the stock encoder sends the stock 1024x384 stream; nothing after the
encoder changes, and `dm.c`'s substitution stays idle (nothing is published, the stream is not
marked alive).

### Switch

Decode mode is the default. The expert menu's "Android Auto map: passed through" writes
`/tsd/var/mst2-cluster-nav/aa-passthrough`, which turns it off; "decoded on the unit" removes
that file (and takes "Built-in map: hidden" back to "kept", see below). A card with the empty
file `mst2-cluster-nav-passthrough` in its root turns it off as well (as `capture.c`'s flag,
`/media/mp000`..`mp009`). Read at every Android Auto connection and, by the lab player, once
a second. Status shows `android auto: on, decoded on the unit`. Install / update removes the
switch file of earlier builds (`aa-decode`) together with the test modes.

### What changes in the Android Auto sink (`aa.c`)

- The discovery offers the cluster display as 800x480 without margins, whatever `aa-res`
  says: the smaller stream, laid out by the phone over the whole picture.
- The SPS is left untouched (no crop rewrite). Access units go to `mcn_decode_put`, with the
  stream's SPS and PPS whenever the phone sent them; the decoder is started by the first SPS
  (coded size from it) and started again if another coded size arrives.
- No focus cycle is run for key frames: the decoder gets the stream from its start. One is
  run only when the decoder reports a lost unit (`mcn_decode_want_key`: queue overflow, a unit
  the renderer dropped, an oversized unit), with the same 2 s limit and the same
  "aa-keycycle-off" switch.
- The HMI gets `video=1` while a picture is being decoded (`mcn_decode_live`), so
  `ClusterAndroidAuto` keeps the cluster's "navigation on the mobile device" notice away. The
  bundle itself needs no change. `KombiMap` hides the built-in cluster map only with GEM
  "Built-in map: hidden" (`hide-map`); in decode mode that must stay off, because
  DisplayManager captures `dpy-6` only while the built-in map redraws underneath (reference
  measurement: the capture fell to 0.4-4 fps when the map was fully covered), and on this
  firmware hiding it stopped the cluster stream altogether. GEM "decoded on the unit" removes
  `hide-map`; `decode.c` logs a warning if it is there at set-up.
- The capture (`capture.c`) records the units as the phone sent them, SPS and PPS put in front
  of a key frame that came without them, so the recording stays playable by the lab player.

### What `decode.c` does

Window, made once per SAL run and never destroyed (the recipe read from the P0478T
DisplayManager binary; DisplayManager's group owners use the same transparent window):

1. `screen_create_context(0)` (application context, no manager rights); the display list is
   read (`DISPLAY_COUNT`, `DISPLAYS`) and every display's id and size logged; the one with
   **id 6 and 1024x384** is taken, by both, never by a fixed index.
2. `screen_create_window` (top level, not in a DisplayManager group), at once the ID string
   `mcnAaClusterClone1` (ends in `Clone1`, no `DispId[`: DisplayManager puts it on display
   index 1, hides it once and forgets it), `VISIBLE 0`, `DISPLAY` = that display, flush.
3. Own group `mcnAaClusterGroup`; `TRANSPARENCY 3`, `COLOR 0`, `BUFFER_SIZE 1x1`, `POSITION
   0,0`, `SIZE 1023x384` (one pixel column of the built-in map stays visible at the right
   edge: the capture is driven by its redraws), `SOURCE_SIZE 1023x384`, `SOURCE_POSITION
   -1023,-384` (the buffer is beside the window: it shows only its transparent colour),
   `ZORDER 500`; one buffer, one post, an event handle, flush.

Renderer, per stream, the stock `H264VideoPlayer` sequence of `sal-video-player.md` section 6:

```
mmr_connect(NULL)
mmr_context_create(conn, "mcn-aa-cluster", 0, 0500)      (a left-over one is opened and destroyed first)
mmr_context_parameters(ctx, {prefetch_mode=-1})
mmr_output_attach(ctx, "screen:/?index=1&wingrp=mcnAaClusterGroup&winid=mcnAaVideoClone1&dstx=0
                        &dsty=0&dstw=1023&dsth=384&overlay=0&zorder=1", "video")
                        (index = the cluster display's index; measured on the car: without it
                        the child is composed on the centre display. On CREATE each window of the renderer
                        is also moved to that display with SCREEN_PROPERTY_DISPLAY.)
mmr_input_parameters(ctx, {VideoDecoderLowLatencyMode=1})
mmr_input_attach(ctx, "mdevpush:/dev/name/local/mcnaamdev?fourcc=H264&video.frame_rate=30
                       &video.depth=16&video.width=800&video.height=480&video.duration=0", "track")
mmr_play(ctx)
name_open("/dev/name/local/mcnaamdev", 0)                 (up to 30 tries, 100 ms apart)
mdev command 5 (absolute PTS = 1), command 2 (format: 2, 'H264', w, h, 16, 30, 1, 0)
per key frame with (new) parameter sets: command 4 (Annex-B SPS+PPS); per access unit:
command 3 (uint64 PTS = monotonic us + 50 ms, then the unit)
teardown: name_close, mmr_stop, mmr_output_detach, mmr_context_destroy, mmr_disconnect
```

Every message is one `MsgSendvs_r` under a `TimerTimeout` of 2.5 s (above the renderer's own
2 s queue wait, so a timeout means its receive thread is gone); the 24-byte header is zeroed
except `cmd` and `len`. The renderer makes its video window a child in our group, so our
context gets its CREATE event: the window is then cropped to the centred 8:3 part of the
picture (`SOURCE_POSITION 0,90`, `SOURCE_SIZE 800x300` for 800x480; constants `VIEW_*` in
`decode.c`) over `0,0 1023x384`, z-order 1 above our transparent parent, and the renderer
laying it out again later is undone at the next tick. There is no `screen:` URL key for a
source rectangle (`screen_writer.so` knows `dstx/dsty/dstw/dsth`, `aspn/aspd`, not `src*`),
hence the child-window properties. The parent is shown only once the first key frame was
pushed and while the mod is active; twice a second its `VISIBLE` and `ZORDER` are read back,
logged when changed from outside, and set again (DisplayManager's one-shot hide). Stream end
hides the window and tears the renderer context down in the stock order; a new stream sets it
up again.

The Android Auto receiver thread only copies units into a bounded queue (128 units, 3 MiB);
one worker thread does the set-up, the sends, the window care and the teardown. On overflow
units are dropped until the next key frame (said once per stream, counted in the statistics)
and a key frame is asked for. A unit the renderer dropped (`-16`: its queue was full for 2 s)
is counted, the following units are skipped until a key frame, five in a row give the stream
up; any other send error, a `name_open` that never succeeds, a missing display or a failing
call of the set-up stop the stream cleanly (one `decode: ... the cluster shows the built-in
map` line) and leave the unit stock until the next stream. Our input is attached only once the
stock Android Auto context plays (`/pps/.../context/SalTechnology::GoogleAutomotiveLink/state`)
or after 3 s: the decoder plug-in opens with static descriptors, so two attaches at the same
moment are avoided.

Lab: with decode mode on, `lab.c` plays `video.h264` as one stream in a loop into the decoder
(the whole file, SPS and PPS handed over with every key frame that carries them; the decoder
is started again at every loop start and at an SPS of another coded size; a lost unit starts
the file over). The file for it is a recording of a phone's real cluster stream (Baseline 3.1,
800x480), on the card as `mst2-cluster-nav-video.h264`, switched on with the expert menu's
"Test video on". No such recording is part of the repository, because it shows the phone
owner's location; `capture.c` records one from your own phone (a card with the empty file
`mst2-cluster-nav-capture-on` in its root), and the same file drives the end-to-end part of
`native/check.sh` (`replays/aa-cluster-800x480.h264` or `MCN_AA_REPLAY`).

### Proven, inferred, confirmed

Proven by disassembly of this unit's binaries: the call sequence, URLs and message formats
(stock player), the display id/size mapping and the CREATE-handler behaviour of
DisplayManager, the Screen property numbers (DisplayManager's own calls). Car-proven on the
sister firmware (same DisplayManager binary) by `mst2-altscreen-carplay`: a second
mm-renderer context decodes beside the stock one and its window on `dpy-6` is captured and
encoded; `overlay=0`; the 1023-pixel width; destroying a window DisplayManager has on its
list of unnamed windows wedges its capture (hence our never-destroyed, named parent).

The points below could only be settled in a car. Points 1 to 4 were confirmed on 2026-11-10
(P0478T): the named parent window survives, the renderer's child in our group is captured and
encoded, the 800x300 source rectangle takes effect (the phone's map, and later the "Google
Maps" logo position, were measured on the cluster), and the mdev commands are accepted. Points
5 and 6 were not measured separately. Listed as they were asked, with the log lines that show
each:

1. That DisplayManager treats our named parent as read from its code (display index 1, hidden
   once, forgotten) and that destroying the renderer's named child at stream end is harmless
   to its capture, unlike the reference's unnamed window. Log: `decode: window was changed from
   outside` (the one-shot hide happened and was undone); DisplayManager's own log: `Making
   screen window [...] visible by default`, no `Empty window title`.
2. That the renderer's child window in our group is composited into the capture (the reference
   used a top-level window). Log: `window joined our group: id 'mcnAaVideoClone1'`, then
   `decode: 10 s: ... dpy-6 composed +N` with N near the phone's frame count and `capture on`.
   If the picture does not appear while frames are pushed, the fallback is the reference's
   top-level URL (`screen:/?index=1&overlay=0&dstx=0&dsty=0&dstw=1023&dsth=384&zorder=500`,
   no crop; one constant in `decode.c`).
3. That the source rectangle on the child window takes effect and is not reset by the renderer.
   Log: `video window shows 800x300 at (0,90) ... rc 0/0/0/0/0/0`; `video window source was
   ... set again` more than a few times means the renderer fights it (then the picture is the
   whole 800x480 squeezed into 8:3).
4. That `name_open` of our mdev name succeeds after `mmr_play` (as stock) and that the decoder
   accepts Annex-B SPS+PPS by command 4 and one access unit per command 3 at 800x480 (stock
   does exactly this for the main video, which is proof enough; only our second instance is
   new). Log: `mdev command 5 ... -> 0`, `command 2 ... -> 0`, `command 4 ... -> 0`, `first
   key frame pushed`.
5. VPU load with three codec instances (main decode, our decode, cluster encode) and the
   composition rate of `dpy-6` with a video window at the phone's rate over the built-in map.
6. The stock-context wait: if `/pps/.../GoogleAutomotiveLink/state` is not where assumed the
   set-up starts 3 s after the first key frame (`stock video context absent after 3000 ms`):
   harmless, the queue holds 4 s.

### Checking an installation

1. Expert menu: "Android Auto map: decoded on the unit" (the default; the release menu has no
   other setting); make sure Status says `built-in map: kept`.
   Optionally a card with `mst2-cluster-nav-capture-on` to record the stream as well.
2. Without a phone first, if a `mst2-cluster-nav-video.h264` recording is on the card:
   "Test video on", reboot, cluster on the map view, wait 20 s. The cluster should show the
   recording's map, scaled to its window, looping (a pause of about 3 s at every loop).
3. With the phone: connect, start Google Maps, cluster on the map view. Expect the phone's map
   within a few seconds of `AA: cluster stream started for the decoder`.
4. Disconnect and reconnect the phone once; switch the cluster to another view and back.
5. "Save diagnostics" and read `mst2-cluster-nav.log`:
   - `AA: cluster map decoded on the unit (800x480, our window on dpy-6)`: the switch took.
   - `AA: stream width 800`, `decode: stream 1 begins, 800x480`: the phone sends 800x480.
   - `decode: display index N: id 6, 1024x384` and `window ready on the cluster display`: the
     display was found and the window made (else: `no display with id 6`: the cluster source is
     another display on this unit, read the display list lines).
   - `decode: stock video context plays after N ms`, then one `-> 0` line per renderer call,
     `output screen:/?...`, `input mdevpush:...`, `name_open ... -> N` (N >= 0), `mdev command
     5/2 -> 0`, `window joined our group: id 'mcnAaVideoClone1'`, `video window shows 800x300
     at (0,90)`, `first key frame pushed`, `window shown`: the whole set-up worked. A `FAILED`
     line names the call that did not, followed by `mmr_error_info` and the reason line.
   - `decode: 10 s: pushed N (bytes), dropped D at the queue, W waiting for a key frame, S
     skipped, send errors E, queue max Q; dpy-6 composed +C, capture on|off`: N near the
     phone's frame count (`AA: frames in the last 10 s`), D/W/S/E 0, C > 0 while the picture
     moves, capture on while the cluster shows the map.
   - `window was changed from outside (visible 0, z-order 0): set again`: DisplayManager's
     one-shot hide, undone; more than once or twice means something keeps hiding it.
   - `the renderer dropped ...`, `queue full`, `AA: video focus sent, mode 2`: units were lost
     and a key frame asked for (the picture pauses on the last frame meanwhile).
   - `stopping (the stream ended)`, `window hidden`, `video window closed`: the end; the next
     `set-up N` after the reconnect logs only failures (the first three set-ups log every call).
   - DisplayManager's log (in the diagnostics if the unit logs it): `Unknown/Unmanaged window
     [...] was closed` at every stream end is expected and must not be followed by a frozen
     cluster picture.

# Findings

The research log behind frame-autopass, kept in the order things were found
(2026-09-29 to 2026-10-01), mistakes and dead ends included. It was written
during development with an AI assistant (see the disclaimer in the README),
so it uses the working names: **fapd** is today's `autopassd` and **fapctl**
is `autopass`. "The user" is the person who directed the project and wore
the headset for every test. Rules described in early sections were often
replaced later; the README describes what ships.

Running log of discovery results. Everything in Step 1 was gathered
read-only: sysfs/procfs reads, log files, `strings` on binaries, and
reading the reference app's source. No device node was opened, no binary
from the reference app was built or run, nothing on the headset was
written.

## 1. Reference app (KominoVR/frame-passthrough-shortcuts, v0.1.0)

Cloned to `ref/frame-passthrough-shortcuts` (gitignored). MIT licence,
copyright "Frame Passthrough Shortcuts contributors". Vendors OpenVR SDK
(BSD-3) and nlohmann/json (MIT).

### Camera source switch

- Gets a private interface: `vr::VR_GetGenericInterface("IVRCameraPassthroughInternal_001")`.
  Present in `/opt/steamvr/bin/linuxarm64/vrclient.so` on our build.
- Calls raw vtable slots on it (no header exists):
  - slot 9: `bool get(void* self, uint8_t cfg[5])`
  - slot 10: `void set(void* self, const uint8_t cfg[5])`
- The 5-byte config is: `[0] enabled/override, [1] stereo, [2] RGB source,
  [3] disable devignetting, [4] sharpening`. Each byte must be 0 or 1.
- Switch = read all 5 bytes, flip byte 2 only, write back, read again to
  verify. Refuses to write if byte 0 is 0 (it never turns the camera on).
- The code comment says it was verified on Frame firmware build 20260918.
  Our headset is on BUILD_ID 20260922.6101926, so it's close but not the
  same build. Vtable slot numbers are the fragile part: a SteamVR update
  that reorders the interface would make slot 10 call the wrong function.
- Translucent/opaque is done through public settings instead:
  `camera.roomViewStyle` (3 = translucent, 4 = opaque) via `IVRSettings`.

### Arcturus presence detection

- Walks `/sys/class/video4linux/*/name` for `arcimx616 *`, then opens
  `/dev/v4l-subdevN` **O_RDWR** and issues a private ioctl `0x800456c1`
  (reads a u32 "sensor connected" flag). It does this every 250 ms.
- The subdevs are held open by XRService. The ioctl is a read, but it
  comes from a second process while XRService is streaming those sensors.
  We should prefer not to copy this: sysfs `led_status` on the arcimx616
  i2c device already reports `present=1 powered=1` without opening any
  device node (see section 3).

### Autostart / registration

- Runs as `VRApplication_Overlay` but never creates an overlay surface.
- `--install` writes `~/.config/frame-passthrough-shortcuts/app.vrmanifest`
  (app key `local.frame.passthrough.shortcuts`, `is_dashboard_overlay:
  true`, both `binary_path_linux` and `binary_path_linux_arm`), calls
  `AddApplicationManifest` then `SetApplicationAutoLaunch(key, true)`.
  SteamVR autolaunches dashboard-overlay apps with autolaunch set.
- Installs under `~/devkit-game/Frame_Passthrough_Shortcuts/` (also a
  Devkit "Non-Steam" library entry). Uses a second, byte-identical
  executable `frame-passthrough-control` for management commands because
  SteamVR identifies apps by executable path.
- Controller input via SteamVR Input action manifest (thumbstick
  double-press / 1 s hold), priority `k_nActionSetOverlayGlobalPriorityMax`.
  Blocked while the dashboard is open.

### Conflict risk with our app (not applicable)

The user has never installed the reference app, so coexistence is out of
scope. Kept for the record only.


The reference app's main loop calls `reconcile_camera_source()` every
20 ms. If it has a saved preference (`camera-preference.json`, written the
first time the user toggles), it **re-asserts that source** whenever byte 2
differs. Running both apps at once means they would fight and flicker.

Mitigations, in order of preference:
1. Detect it (`VRApplications()->GetApplicationProcessId("local.frame.passthrough.shortcuts")`
   or its `runtime.json`) and back off / warn. Our app then owns only the
   auto mode and leaves manual RGB toggles to theirs, or
2. Tell the user to use ours instead of theirs for RGB/mono (theirs is still
   fine for translucent/opaque; that path doesn't touch byte 2), or
3. Remove its saved preference file, which makes its reconcile a no-op
   until the next manual toggle. (Touches their state; not preferred.)

Status on this headset: **reference app is not installed** (no
`~/devkit-game`, no config dir, no vrappconfig).

## 2. OS / runtime

| Item | Value |
| --- | --- |
| OS | SteamOS `holo`, VARIANT_ID `vr`, VERSION_ID 0.3.0, BUILD_ID 20260922.6101926 |
| Kernel | 6.18.0-gfbdbca41fd45, aarch64 |
| SoC | Qualcomm SM8650 (Snapdragon 8 Gen 3), 8 cores, 15.6 GB RAM |
| SteamVR | `/opt/steamvr` (part of the read-only OS image), `bin/version.txt` = 1789606310 |
| OpenXR | `~/.config/openxr/1/active_runtime.json` -> `/opt/steamvr/steamxr_linuxarm64.json` (SteamVR) |
| OpenVR | `~/.config/openvr/openvrpaths.vrpath`, runtime `/opt/steamvr`, no external drivers |
| User | `steamos` (uid 1000), groups include `video`, `render`, `cdsp` |
| CV driver | `/opt/steamvr/drivers/cv` (driver_cv.so + XRService + libArcturusPerception.so) |

Notable: XRService (Valve's tracking / passthrough service) is built on
the `arcturus-xr` codebase, and it names the colour module
`"Arcturus Camera V3"`. The colour module is therefore handled natively by
Valve's stack, not by a third-party driver.

Public OpenVR camera API present in vrclient: `IVRTrackedCamera_006`.
driver_cv implements `IVRCameraComponent_003`.

## 3. Cameras

All camera sensors sit behind Qualcomm CAMSS (`/dev/media0`). V4L2 nodes:

| Subdev | Sensor | I2C | Role (inferred) |
| --- | --- | --- | --- |
| v4l-subdev28 | arcimx616 0-0010 | cci0 i2c-0 | Arcturus colour, one eye |
| v4l-subdev29 | arcimx616 0-001a | cci0 i2c-0 | Arcturus colour, other eye (owns LED effect/status attrs) |
| v4l-subdev30 | og01a1bx 4-0060 | cci1 i2c-4 | OmniVision OG01A1B mono global shutter, supplies "l0" |
| v4l-subdev31 | og01a1bx 4-0036 | cci1 i2c-4 | same, "r0" |
| v4l-subdev32 | og0ve10x 5-0060 | cci1 i2c-5 | OmniVision OG0VE1B mono global shutter, "l1" |
| v4l-subdev33 | og0ve10x 5-003e | cci1 i2c-5 | same, "r1" |

Four mono sensors (two pairs, l0/r0 and l1/r1) plus two colour. Which mono
pair feeds passthrough vs tracking-only is not yet confirmed; the log says
"Using 4 camera positions for CAD<>CAL alignment". All sensors report
runtime PM `active`.

Ownership: **XRService (pid 2574) holds `/dev/media0`, all six sensor
subdevs, and capture nodes video0/3/6/7/9/13.** Nobody else touches the
sensors. Frames go from XRService to vrcompositor as dma-bufs (XRService
holds ~290 dmabuf fds) and are drawn by `CTrackedCamera` in vrcompositor
with shaders `tracked_camera_reprojection_simplified_{rgb,monochrome}_nv12`
(so both feeds arrive as NV12). Passthrough is reprojected onto a room
depth mesh ("Augmented passthrough geometry", "RoomView mesh" block queues).

Other video nodes:
- `/dev/video99` "SteamVR": v4l2loopback, fed by `/opt/steamvr/bin/linuxarm64/v4l2cam --output=99`.
  Format RGB3 1920x1080@30. v4l2cam uses `IVRHeadsetView`, so this is the
  **composited headset view** (a virtual webcam), not a raw camera.
  Readable by group `video` (we are in it).
- video22/23: Iris hardware video decoder/encoder.

Arcturus i2c sysfs attributes (readable without opening a device node):
`0-001a/led_status` = `present=1 powered=1 effect=static ... last_error=0 override=0`.
This is a cheap, non-invasive presence check.

Direct V4L2 capture from our own process is **not viable**: the sensors
and the CAMSS pipeline are exclusively in use by XRService, and grabbing
them would risk tracking. Frames must come through SteamVR.

## 4. Light sensing

### Hardware ALS: exists, probably unusable for room light

`iio:device2` = Vishay **VCNL4040** (ALS + proximity) at i2c-6 0x60, on
the same bus as the two speaker amps. `in_illuminance_raw` and
`in_proximity_raw` are world-readable in sysfs. Nothing holds
`/dev/iio:device2`; `proxmicmute` references the proximity path.

Current readings (headset idle, display backlight 0): illuminance 0,
proximity ~3 (near-level 220). This is almost certainly the face-presence
sensor pointing into the facial interface, which would read ~0 whenever
the headset is worn. **Unconfirmed**; needs a simple test (point the
front at a lamp vs the lenses at a lamp, reading sysfs).

### Camera exposure / gain metadata: best candidate

- XRService logs `Left/Right camera is now too dark or covered /
  well-exposed / too bright` for the mono cameras, and `HmdAnalogGainMax
  1.25`.
- XRService carries colour metadata (`CameraColorFrameMetadata.h`) with
  fields `exposureMs`, `isoSpeed`, `redGain/greenGain/blueGain`,
  `whiteBalanceGains`, `useAutoExposure`, `useAutoWhiteBalance`.
  Session settings push `captureSessionSettings.passthroughCameras.useAutoExposure`.
- Public route: `IVRTrackedCamera_006` frame header
  (`CameraVideoStreamFrameHeader_t`) has `ulFrameExposureTime` plus
  frame sequence and pose. Whether that is populated on Frame, and whether
  it describes the colour or mono feed, is **untested**.
- Auto-exposure time alone saturates in the dark (it hits max exposure,
  then gain rises). A usable light metric is `exposure x gain` or, failing
  gain, exposure plus mean frame brightness.

### Mean frame brightness: fallback

Via `IVRTrackedCamera` frame buffers if accessible. `/dev/video99` also
works in principle but it is the composited view (content and overlays
bias it), so only a last resort.

### Does the RGB feed stay readable while mono is selected?

The reference README says the RGB sensor stays powered. Consistent with
what we see (both arcimx616 `rpm=active`, `powered=1`). Whether its
frames are still reachable from a client while byte 2 = 0 is **untested**.
That determines whether we can detect "the lights came back on" while in
IR mode (otherwise we need the mono feed's exposure for that direction).

## 5. Phase 2 signals

- Camera frames: only via SteamVR (`IVRTrackedCamera_006`), or not at all.
  Unknown whether it exposes mono, colour, or both, at what resolution,
  and whether the frame header pose is populated.
- Compositing: passthrough is drawn inside vrcompositor with fixed
  shaders from the read-only image. Controls available: `roomView`,
  `roomViewStyle` (translucent/opaque), the private 5-byte config
  (stereo/RGB/devignette/sharpen), and `camera.monochromeTintHue` (0.67
  default, a single global tint for the mono feed). There is no public
  per-pixel blend mode; `IVROverlay` layers are alpha-blended only.
- SteamVR already maintains a room depth mesh for passthrough
  reprojection and XRService keeps a SLAM map (`serializedmap/`, 144
  keyframes / 2115 map points in the stats dump), both internal.

## Open questions (need live but non-mutating tests, pending approval)

1. VCNL4040 placement: read sysfs while the user shines a light at the
   front vs into the lenses.
2. `IVRTrackedCamera_006` probe as a background OpenVR client:
   `HasCamera`, frame sizes per frame type, acquire a stream for a few
   seconds, log header (exposure, sequence, pose), mean luma, which feed it
   is, and whether it changes when the source switches.
3. `IVRCameraPassthroughInternal_001` slot 9 read-only (get config) to
   confirm the 5-byte layout on this build.

## 6. Build and deploy setup (2026-09-29)

- Cross-compiling on the PC with Zig 0.16.0 (`zig c++ -target
  aarch64-linux-gnu.2.35`; the headset has glibc 2.39). libc++ is linked
  statically, so binaries only need glibc. `scripts/fetch-deps.sh` fetches
  Zig (sha256-checked) and the OpenVR SDK 2.15.6 (commit 0924064) into
  gitignored dirs. `make test` runs host tests, `make device` builds for
  the headset.
- Deploy target on the headset: `~/fap/bin/`.
- SteamVR runtime reports itself as **2.17.10** in client logs.

## 7. Public tracked-camera API, first live probe (headset idle)

Run with `camera_probe` as a Background client while the headset was
idle (display backlight 0, not worn, passthrough not showing):

- `Prop_HasCamera_Bool` true, but `Prop_NumCameras_Int32` = 0 and
  frame layout / stream format / firmware properties are unknown.
- `IVRTrackedCamera::HasCamera` true. `GetCameraFrameSize` and
  `GetVideoStreamTextureSize` fail (`OperationFailed`) for all three
  frame types, before and after acquiring.
- `AcquireVideoStreamingService` succeeds, but
  `GetVideoStreamFrameBuffer` on that handle returns `InvalidHandle`.
- vrserver logged nothing about the camera request.
- Settings as stored: `camera.enableCamera` true, `roomView` 2,
  `roomViewStyle` 0, `enableConstructRoomView` true,
  `monochromeTintHue` 0.67.

Not conclusive yet: it may only work while passthrough is actually
running. Next: repeat with the headset worn and passthrough visible. If
it still fails, the public frame API is not wired up on Frame and light
sensing has to come from elsewhere.

## 8. Light sensor torch test and worn probe (2026-09-29 21:15)

VCNL4040 sampled at 5 Hz for 60 s while the user wore the headset in a
lit room, then shone a phone torch at the front and then the inside:

- Normal lit room, worn: raw 0-5 (0-0.5 lux at scale 0.1).
- Torch spike 1 (~15-20 s): raw up to 2200 (220 lux).
- Torch spike 2 (~33 s): raw up to 223.
- Proximity sat around 12-13 while worn and dropped to ~2-5 while the
  headset was moved about. It never came near the 220 near-level.

Verdict: whichever way it faces, a lit room reads under 1 lux with the
fixed 80 ms integration time (changing it needs root). **Not usable as
a room-light sensor.** Dropped from the plan.

The "worn" camera_probe rerun happened around the moment passthrough
was closed ("Passthrough cameras paused" 21:16:57), so it does not
settle whether IVRTrackedCamera works while passthrough runs. Same result
as idle: sizes fail, frame read returns InvalidHandle. Needs one clean
retry.

XRService's own log (`~/.local/share/Steam/logs/xrservice.txt`, a
symlink to the current session log) is live and readable. It records
`Passthrough cameras paused/resumed`, `Tracking cameras streaming
paused/resumed` (headset taken off / put on), and, under the torch, bursts
of `Max number of iteration reached when estimating the CCT from grey
world gains` (colour AWB struggling). No periodic exposure values.

## 9. Private interface mapped from this headset's vrclient.so

Copied `/opt/steamvr/bin/linuxarm64/vrclient.so` to the PC (read-only)
and located the implementation through RTTI and R_AARCH64_RELATIVE
relocations, without using the reference app's code:

- Class `CVRCameraPassthroughInternal` (typeinfo 0x616948), implementing
  `vr::IVRCameraPassthroughInternal`, interface string
  `IVRCameraPassthroughInternal_001`.
- Vtable at 0x60cc90 with **17 methods**:

| idx | addr | approx size |
| --- | --- | --- |
| 0 | 0x1a4728 | |
| 1 | 0x1a5670 | |
| 2 | 0x1a4a78 | |
| 3 | 0x1a4cd0 | |
| 4 | 0x1a4df8 | |
| 5 | 0x1a4f30 | ~1.4 KB (largest) |
| 6 | 0x1a44b0 | |
| 7 | 0x1a46d0 | |
| 8 | 0x1a4b38 | |
| 9 | 0x1a49d8 | ~160 B |
| 10 | 0x1a4bf8 | ~216 B |
| 11 | 0x1a4708 | |
| 12 | 0x1a4b98 | |
| 13 | 0x1a4968 | |
| 14 | 0x1a5498 | |
| 15 | 0x1a4498 | |
| 16 | 0x1a45c0 | |

- Strings owned by the class: `Failed to import the camera frame
  dma-buf`, `Failed to reference the shared image resource`, `...shared
  spline distortion resource`, `...spline distortion resource file
  descriptor`, plus shared-memory names `VR_CameraPassthroughState` and
  `VR_CameraPassthroughMutex`.

So this private interface is not only the source toggle: it also hands
out **camera frames as dma-bufs** together with the lens distortion
(spline) data. That is potentially the frame access we need for the
light level (Phase 1) and for Phase 2. The config state lives in a shared
memory block guarded by a named mutex.

Next: disassemble the 17 methods to learn their signatures. The host has
no arm64 disassembler yet.

## 10. The public camera stream blanks the user's passthrough (21:23)

Clean retry with the headset worn and passthrough running (XRService
logged `Passthrough cameras resumed` at 21:23:02; probe ran 21:23:11 to
21:23:16):

- Same result: frame sizes fail, frame reads return `InvalidHandle`.
  **The public IVRTrackedCamera frame API is not usable on Frame.**
- Side effect reported by the user: every probe run turned their
  passthrough **black** until they toggled passthrough off and on.
  vrcompositor logged, at the moment the probe disconnected:
  `[CTrackedCamera] : Depth mesh block queues disconnected successfully.`
  The compositor's own passthrough renderer (CTrackedCamera) shares the
  tracked-camera service; a client's acquire/release (or its disconnect
  while holding the stream) tears that renderer down. The earlier 21:16
  `Passthrough cameras paused` was very likely caused by the probe too.
- Tracking was not affected (no SLAM or VIO resets in the stats).

Action taken: the probe binary was deleted from the headset and the
stream acquisition code removed from `tools/camera_probe.cpp`; it now
only reads properties and settings.

Rule from here on: any live call into SteamVR's camera stack is assumed
able to disturb the user's view. Before running one, say so, have the
user ready to toggle passthrough, and prefer analysing the binaries
offline first.

## 11. Private interface: get/set config confirmed by disassembly

`tools/disasm.py` (capstone, in `toolchain/venv`) disassembles the
headset's own vrclient.so. `this+0x18` holds the shared-state accessor;
helpers at 0x258500 / 0x258578 / 0x258508 are lock / get-pointer / unlock
of the `VR_CameraPassthroughState` block (guarded by
`VR_CameraPassthroughMutex`).

- Slot 9, `bool GetConfig(uint8_t cfg[5])`: under the lock, copies
  state bytes 2..6 into `cfg`, returns `state[2] != 0` (the enabled
  byte). With `cfg == nullptr` it returns that flag without the copy.
- Slot 10, `void SetConfig(const uint8_t cfg[5])`: under the lock,
  compares `cfg` with state bytes 2..6 and returns without doing anything
  if equal. Otherwise writes all 5 bytes, unlocks, and broadcasts an event
  built from the rodata pair (812, 0xffffffff): event type 812, device
  index -1 (all), via a client singleton's vtable +0xc0.
- So the layout is: state[2] enabled, [3] stereo, [4] RGB source,
  [5] disable devignetting, [6] sharpening (byte meanings from the earlier
  analysis; the code itself only shows a 5-byte block starting at +2).
- Writing is a no-op when unchanged, so a periodic re-assert is cheap but
  still wasteful; our app should only write on a decision change.

## 12. VR_CameraPassthroughState is a readable tmpfs file

- vrclient opens it (at 0x1a5adc) with size **0x6200 = 25088 bytes**,
  plus a named mutex `VR_CameraPassthroughMutex`.
- On the headset it is `/dev/shm/u1000-Shm_78c2379b` (mode 0777, owner
  steamos), mapped by vrcompositor, vrserver and v4l2cam. The hash in the
  name is not yet derived, so tools find it by its unique size.
- Reading the file is completely passive: no SteamVR call, no lock, no
  device. It cannot affect passthrough or tracking.

Static layout (snapshot at 21:26 with passthrough paused):

| offset | content |
| --- | --- |
| 0x00-0x01 | 01 00 (header) |
| 0x02-0x06 | the 5-byte config: enabled, stereo, RGB, disable devignetting, sharpening. Read as `01 01 01 00 01` (RGB active, as expected with the Arcturus attached) |
| 0x14 | stream 0 (mono): count 4, 1056 x 1024. Per-frame records mostly empty in this snapshot |
| 0x20e8 | stream 1 (colour): count 4, 2464 x 2464. Per-camera blocks 0x1040 apart, per-frame records 0x104 (260) bytes apart |

Colour per-frame record fields found so far (relative to the float
1/2.2 gamma marker at record start + 0xd8):

- record start: frame ids, two float64 timestamps (~5200 s, same clock),
  fx fy cx cy (~1478, 1479, 1369, 1250 for 2464 px), 4 distortion
  coefficients, rotation matrices, an identity 3x3.
- after the marker: 0, 0, 1.0, 1.0, **a per-frame value (0.861, 0.843,
  0.597)**, an int 1, **0.000331492 twice (candidate: left/right exposure
  time in seconds, 331 us)**, 1.0, 1.0 (candidate gains), 0, then what
  looks like the next record's owner pid (0x0a12 = 2578, vrcompositor)
  and a sequence number.

Hypotheses to test live: g6/g7 = exposure time, g8/g9 = gain, g4 =
something scene-dependent (AE target, brightness or a white balance
figure). Also: do the mono records fill while colour is selected (that
decides whether we can see the IR side's exposure)?

`tools/shm_sampler.py` samples the file at 20 Hz and logs per-record
fields to CSV, plus optional raw snapshots for offline re-parsing.

## 13. Live capture: a light signal in shared memory (21:32-21:34)

95 s passive capture (`shm_sampler.py`, 20 Hz, raw snapshots kept) with
the headset worn and colour passthrough running throughout (XRService
resumed 21:31:53, no pause during the capture). User: ~20 s lit, ~30 s
lights off, then lights on.

- Colour per-frame records update at ~13 Hz per slot. Config stayed
  `01 01 01 00 01` (RGB) the whole time.
- **g4 (float at gamma marker + 0x14; e.g. 0x220c, 0x324c) tracks room
  light:** 0.8-1.0 lit (saturates at 1.0), fell from 0.8 to 0.0 over
  t=13-18 s, stayed 0.00-0.03 in the dark (one 0.14-0.37 blip at
  t=35-38 s, likely a screen or light leak), rose from 0.2 to 0.8 over
  t=48-56 s, back to 0.85-0.95. Identical in both camera blocks.
- A second field at 0x2414 / 0x3454 behaves like a slow, sparsely
  updated version of the same (0.81 lit, 0 dark, 0.67-0.80 lit again,
  updates every few seconds).
- The candidate "exposure" pair (0.000331) and "gains" (1.0) never
  changed: that guess was wrong.
- Several rotation-matrix elements also separated lit from dark, but
  those are head pose (the user moved differently in the dark), not
  light. Treat any pose-derived field as a confound.
- **The mono stream region (0x14-0x20e8) never changed at all**: mono
  records are not published while RGB is selected.

What g4 is, exactly, is unknown. It behaves like a normalised "enough
light for colour" figure (clamped 0..1), which is exactly the colour to
IR trigger we want. It has no gradation below the point where colour
fails, so it cannot on its own tell "dim" from "pitch black".

Open question that decides the IR to colour direction: do the colour
records keep updating while mono is selected? That needs a real source
switch (private SetConfig), which changes what the user sees.

## 14. fapctl: our own reader and switch

- `src/passthrough_state.*`: passive parser of the shared state (config
  bytes, newest colour record's timestamp and light value). Tested on two
  real snapshots from the live capture (`tests/fixtures/state_{lit,dark}.bin`:
  light 0.884 and 0.000).
- Trap found while writing it: the gamma marker is stored as
  `0x3ee8ba2f` (1/2.2 in double, rounded to float). `1.0f / 2.2f` in C++
  gives `0x3ee8ba2e`, so the marker must be matched by its bit pattern.
- `src/private_camera.*`: get/set through `IVRCameraPassthroughInternal_001`
  slots 9/10. Before calling, the first 6 and 7 instruction words of the
  two methods are compared with those in this headset's vrclient.so
  (position-independent words only). Any SteamVR update that changes them
  makes the tool refuse instead of calling into unknown code.
- `tools/fapctl`: `status [SECONDS]` (passive), `connect-test` (VR_Init
  as Background, wait, shut down), `get`, `set rgb|mono` (changes only
  the RGB byte, refuses if the camera is not enabled, verifies read-back).
- Only `status` has been run on the headset so far (21:36): config
  `enabled=1 stereo=1 rgb=1 disable_devignetting=0 sharpening=1`.

## 15. First real switch test (21:41:48-21:42:56, user approved)

Headset worn, colour passthrough running (resumed 21:41:26). Timeline
from the run start: 0 s `fapctl connect-test`, 10 s `fapctl set mono`,
55 s `fapctl set rgb`, passive sampler throughout. User: lights on, off
at ~25 s, back on ~5 s early (~35 s).

- Both writes succeeded and read back exactly; only the RGB byte changed.
- **No blanking**: vrcompositor logged only `[CTrackedCamera] Late
  initialization successful` at each switch (21:41:49, 21:42:34), and none
  of the teardown seen with the IVRTrackedCamera probe. Connecting and
  disconnecting as a Background client alone did nothing visible in logs.
- XRService logged `Transition to IMUFallback` at 21:41:54.8 (6 s after
  the mono switch), recovered at 21:41:58.3. Not proven related: the same
  transition occurs 14 times in today's log, including at 21:32:58 during
  the passive capture with no switching, and on every headset off/on.
  Needs watching in future tests.
- **In mono the colour records stop updating** (newest colour timestamp
  frozen at the switch) and the mono region starts updating. Back on RGB,
  colour records resume immediately (g4 0.72 then 0.40-0.72).
- **Nothing in the shared state tracks light while mono is showing.**
  Across all 118 words that varied during mono, the only lit/dark
  differences are pose elements (separation < 1.4). Mono records carry no
  gamma marker and no exposure-like field; a pair at 0x138/0x23c/0x1178/0x127c
  is 0 except a blip at ~30 s.
- XRService's colour white-balance warnings (`estimating the CCT from grey
  world gains`) continued through the mono period (21:42:04-21:42:33), so
  XRService keeps processing colour frames while mono is displayed; it just
  does not publish them to this block.

Consequence for Phase 1: colour to IR is solved (g4 from shared memory,
passive). IR to colour has no signal yet. Options:
1. Find the colour statistics in XRService's other IPC (the world-writable
   `/dev/shm/XR_ServerResponse_*` / `XR_ClientRequest_*` buffers), still
   passive.
2. A brief periodic colour "peek" (switch to RGB for ~1-2 s, read g4,
   switch back). Visible flash; last resort.
3. Only auto-switch colour to IR, and return by controller.

## 16. Hunting a light signal usable in IR mode

User saw nothing unusual during the 21:41 switch test (no blink, no view
jump); the IMUFallback at 21:41:54 is logged as unrelated for now (user's
guess: a controller idling off), still to be watched.

The `XR_*` IPC buffers are mapped by XRService, vrserver and vrcompositor
(`XR_ServerResponse_High_Data` holds JSON status like `TRACKING_LEDS`
occurrences). XRService maps **73 shm files, 67.7 MB** in total.

Tools:
- `tools/shm_capture.py`: passively snapshots every shm file a process
  maps (small files every 0.25 s, >1 MB every 2 s, zlib-compressed),
  plus `--also` extra files. ~16% of one core, ~1 MB/s on disk.
- `tools/shm_analyse.py`: offline; takes g4 from the passthrough state
  as ground truth during the RGB phase, correlates every 32-bit word of
  every other buffer with it, and prints the best candidates' ranges in
  the RGB and mono phases. Verified on a synthetic capture with one
  planted signal among 1024 random words (found at r = 1.000, nothing
  else above 0.8).

Plan: one run with an RGB phase (lit / dark / lit) then a mono phase
(lit / dark / lit), then back to RGB.

## 17. Whole-process capture 1 (21:52-21:54): pose confound again

Run: user lights on 0-30 s / off 30-60 s / on 60-90 s; `fapctl set
mono` at 45 s, `set rgb` at 75 s (fixed times, not detection). Capture
of all 73 XRService shm files plus the passthrough state: 105 MB, 380
state snapshots. No IMUFallback during the run.

- g4 ground truth (RGB phase): ~0.9 lit, ramp 0.54 to 0 over 20-28 s,
  0 while dark.
- 112 words correlate with g4 at |r| >= 0.7. A stricter test (must step
  the same way at lights-off in RGB and at lights-on in mono, and be
  steady within segments) still ranks pose-like values top:
  `u1000-Shm_bc4005bc+0xef194` / `u1000-Shm_4c4bddef+0xa700,+0xb724,+0xbb00`
  (8.08 lit, 6.44 dark, 6.35 mono dark, 8.06 mono lit: looks like an
  unwrapped angle, 2pi..2.6pi), `u1000-Shm_7f04d645+0x1e0` and copies,
  `XR_ServerRequest_VLow_Data+0x90/+0x110`, plus rotation-matrix blocks.
- Cause: the user stands at the light switch (facing it) whenever it is
  dark and elsewhere when lit, so head pose is perfectly confounded with
  light in both phases. The step test cannot separate them.

Next capture must decouple pose from light: the user stays still at the
switch, looking at one spot, for the whole run.

## 18. Capture 2 (22:01-22:02): head still, light switched by Home Assistant

The user sat still looking at one spot. I toggled the room A light (LIFX
Mini, via the user's Home Assistant in their Chrome): **off 22:01:31
(T0+30.4 s), on 22:02:03 (T0+61.9 s)**, times from HA's activity log.
`set mono` at T0+45, `set rgb` at T0+75. No IMUFallback.

- g4 went 1.00 to 0.44 (not 0: the room kept other light), back to 1.00.
- With the head still, pose-derived words no longer rank (score < 2).
- One group stood out, stepping exactly with the light in **both** RGB
  and mono: xyz triplets in `u1000-Shm_7f04d645` (24,002,048 bytes, about
  4 x the "RoomView mesh data block queue of size 6000000"), e.g. +0x5c728c
  (0.128, -0.371, 0.421) lit vs (0.100, -0.289, 0.328) dark. Fell over
  30-34 s and returned at 64 s while mono was displayed.
- **It is the passthrough depth mesh, not a light meter.** The block is
  smoothly varying vertex triplets; 174,758 of ~207k nonzero words changed
  >2% between lit and dark (dark/lit ratio median 0.95, 5-95% 0.71-1.11).
  XRService's stereo depth estimate changes with the lighting, so the whole
  mesh shifts. Scene- and pose-dependent, so not usable as a light signal,
  although it shows the mono pipeline does react to room light.

Static look at the colour sensor driver (built into the kernel): symbols
`arcimx616_get_ctrl`, `arcimx616_set_ctrl`, `arcimx616_ctrl_read_ops`,
`arcimx616_ctrl_temperature`, `arcimx616_private_ioctl`, modes
3820x2464 and 1640x1232 binned. So the subdev has V4L2 controls; whether
exposure/gain are among them (and are updated by XRService's AE while mono
is shown) is unknown without querying the device.

## 19. Colour sensor V4L2 controls (read-only query, 22:08:57)

User approved opening the Arcturus sensor subdevs read-only.
`tools/subdev_ctrls` opens the node O_RDONLY and issues only
VIDIOC_QUERY_EXT_CTRL / VIDIOC_G_EXT_CTRLS. Headset idle, passthrough
paused since 22:03:49. No XRService or kernel log entries followed.

Both `arcimx616` subdevs (28 = 0-0010, 29 = 0-001a) expose the same 5:

| id | name | type | range | flags | value (28 / 29) |
| --- | --- | --- | --- | --- | --- |
| 0x00980900 | Brightness | int | 0..16777215 | 0x1020 (slider, not volatile) | 77841 (0x13011) / 12305 (0x3011) |
| 0x0098f900 | Temperature | int | 0..2^32-1 | 0x1080 (volatile) | 37 / 37 |
| 0x009f0901 | Link Frequency | intmenu | 0..0 | 0x1004 (read-only) | 0 |

No standard Exposure or Analogue Gain control. "Brightness" is a 24-bit
non-volatile control, so it holds the last value someone set with
S_CTRL, presumably XRService. The two sensors differ only in bit 16.
Hypothesis: XRService packs exposure and/or gain into it. Needs a live
test to see whether it moves with light, and whether XRService keeps
writing it while mono is displayed.

## 20. Run 3 (22:12:29-22:14:04): sensor controls do not track light

Head still; HA light off 22:13:02 (T0+33), on 22:13:31 (T0+62); mono at
T0+45, RGB at T0+75. Both arcimx616 subdevs polled read-only at 5 Hz.
No IMUFallback, no log noise.

- **Brightness stayed constant** on both sensors for the whole run
  (77841 / 12305) through light, dark, colour and mono. It is static
  configuration, not auto-exposure. Temperature rose 47 to 54 C (warm-up).
- So XRService does not run AE through standard V4L2 controls; it must go
  through `arcimx616_private_ioctl` or the ISP. Reading those would need
  reverse-engineering the driver and touching the sensor bus while
  XRService streams: not pursued.

## 21. Peek latency: colour light value is valid immediately after a switch

From capture 2 (state file every 0.25 s): after `set rgb` at T0+74.8, the
very first snapshot already held a fresh colour record (timestamp jumped
from the frozen 7337.302 to 7367.388) with the correct light value 1.000,
and it stayed consistent afterwards. There is no AE settling after the
switch, which fits XRService keeping colour auto-exposure running while
mono is displayed (its colour AWB log lines also continue in mono).

So a "peek" (switch to RGB, wait for the first fresh colour record, read
g4, switch back) could be short: bounded by the switch plus one colour
frame (~13 Hz, so roughly 80 ms) plus compositor re-init. Exact latency
still to be measured at high polling rate.

Status of IR-to-colour detection, all passive routes tried:
- shared passthrough state: colour records frozen in mono (section 15)
- all 73 XRService shm buffers: only the depth mesh reacts (section 18)
- sensor V4L2 controls: static (this section)
- ALS: too insensitive (section 8); public camera API: dead and harmful
  (section 10)

## 22. Is there an official way? (web search, 2026-09-29)

No. Checked Valve's Steamworks Steam Frame docs, SteamVR 2.17 release
coverage, Arcturus Vision's site, the Road to VR review and a VR.org
developer piece:

- Valve's Steam Frame developer docs (setup, Unity, Unreal, Godot, custom
  engines) never mention passthrough, environment blend modes, camera
  state or whether a colour module is attached. Developer mode offers
  SSH/ADB/RDP, beta branches, debugging and performance overlays.
- SteamVR does not implement OpenXR passthrough extensions; the only
  camera path is OpenVR's tracked-camera interface, which returns nothing
  on Frame (section 10). The community OpenXR passthrough layer
  (Rectus/openxr-steamvr-passthrough) relies on that same interface.
- SteamVR 2.17 (released 2026-09-11) added a Quick Access slider for
  environment and camera brightness: a control, not a meter.
- Arcturus Vision: no public SDK/API. The module "instantly takes over"
  passthrough on plug-in; reviews and the site describe no low-light
  fallback or colour/mono switch.

Everything this project does (the source switch included) is therefore
through private interfaces, and will need re-verification after SteamVR
updates (fapctl's fingerprint check exists for that reason).

Sources: partner.steamgames.com/doc/steamhardware/steamframe/setup,
vr.org/articles/steam-frame-color-passthrough-arcturus-vision-149-mixed-reality-developers-2026,
roadtovr.com/arcturus-vision-camera-review-steam-frame-passthrough-add-on/,
gamingonlinux.com/2026/09/steamvr-2-17-arrives-ready-to-go-for-the-steam-frame/,
arcturus.vision/howto, github.com/Rectus/openxr-steamvr-passthrough

## 23. Private interface: producer and consumer halves (offline RE)

`tools/disasm.py` gained `--until ADDR` (linear disassembly of a range),
since loops end the heuristic walk early.

- **Slot 0** `SetGraphicsDevice(desc*)`: `desc->type` must be 2 (Vulkan;
  otherwise logs "only Vulkan graphics devices are supported");
  `desc->data` points at {VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
  u32 queueFamilyIndex}, stored at this+0x50..0x70; sets this+0x74 once
  the device is usable. Returns 0 on success, 1 on bad arguments.
- **Slot 1** is the **producer**: `(stream, desc*, images*)`, requires
  slot 0 first (returns 2 otherwise), `desc` starts with a count <= 16,
  per-image structs are 0x68 bytes. For each image it sets up a "shared
  camera frame" (0x1a4808) and the "shared spline distortion images"
  (0x1a5140), then under the state mutex copies a new 0x20d4-byte stream
  block into the shared state (stream 0 at +0x14, stream 1 at +0x20e8) and
  increments the stream's generation counter. The records' owner pid is
  vrcompositor, so **the compositor publishes its camera frames to other
  clients through this interface**.
- **Consumers** (all take `stream` 0 = mono, 1 = colour, and check a
  per-stream "imported" flag at this+0x78 / this+0xc0):
  slot 2 `(stream, u8* out)` returns the stream's available flag (+0x40);
  slot 3 `(stream, eye <= 1, out*, size)`; slot 4 `(stream, eye <= 1,
  frame < count)`; slot 6 `(stream, eye <= 1, out*)` calls four virtuals
  on the imported per-eye image object (vtable +0xb0, +0xb8, +0x40,
  +0x48) and writes the results (two handles, then width/height-like
  values) to `out`.

So a Vulkan client can import the frames of the stream that is currently
displayed. Only one stream is published at a time (section 15), so in IR
mode we would receive mono frames.

Before building the Vulkan import, a cheap feasibility test: does the
**mono passthrough image** itself change measurably when the room light
comes on, or does XRService's auto-exposure flatten it? `/dev/video99`
(SteamVR's virtual webcam of the headset view, RGB3 1920x1080@30, group
`video`) shows the displayed passthrough. Reading it consumes an existing
output and changes no SteamVR state.

`tools/view_grab` reads `/dev/video99` as a normal V4L2 capture consumer
(mmap buffers) and prints mean / p5 / p50 / p95 luma and the fraction of
clipped pixels, plus optional 1/8-scale PGM thumbnails. First run (22:24,
headset idle): 1920x1080 RGB24 frames arrive at the expected rate, all
black because nothing is displayed. No SteamVR log lines resulted.

## 24. Run 4 (2026-09-30 22:40-22:42): image grain detects light while in IR

Mono selected at T0+5 s, back to RGB at T0+115 s. Room A light via HA:
off 22:40:48, on 22:41:18, off 22:41:38, on 22:41:58 (T0+31.8, 61.8,
81.8, 101.8). `view_grab` read `/dev/video99` at 5 Hz with a 1/8-scale
point-sampled thumbnail every 5 s. No IMUFallback. The user had Steam
panels open in the middle of the view, and their content changed during
the run.

Whole-frame brightness (1 s means) in mono:
- Lights off: a sharp ~1 s dip (p50 55 to 11, 41 to 17), auto-exposure
  recovers within ~2 s. Lights on: a short spike, then settles.
- Steady state: mean is largely equalised by AE, but contrast differs:
  lit p5/p95 ~7-8 / 83-86, IR only ~14 / 65.
- Panel content changes (t=11-30 s) move the whole-frame stats as much as
  the light does, so whole-frame brightness is not reliable on its own.

**Grain (median absolute Laplacian) in the top 35% of the thumbnail
(ceiling and upper walls, clear of panels):**

| condition | samples | grain |
| --- | --- | --- |
| lit, mono (5-31 s) | 6 | 7, 7, 7, 8, 7, 7 |
| dark, mono (36-62 s) | 6 | 15, 15, 15, 14, 15, 15 |
| lit, mono (67-82 s) | 4 | 6, 6, 6, 6 |
| dark, mono (87-98 s) | 3 | 14, 14, 14 |
| just after on (102.8 s) | 1 | 9 (transient) |
| lit, mono (108-113 s) | 2 | 6, 6 |

A clean 2x separation, reproduced over two cycles. Cause: with only
the IR illuminators the mono sensors run high analogue gain, which shows
as grain; AE can equalise mean brightness but not noise. This is a
**usable IR-to-colour signal**, obtained passively from the displayed view.

Caveats to resolve before relying on it:
- Measured on thumbnails of the composited view (reprojected, sharpened,
  with overlays). Needs a proper full-resolution estimator and a choice
  of region robust to panels and head motion.
- Only two light levels tested (room A light on/off with some other light
  present). Needs a calibration sweep of intermediate levels to map grain
  to the colour-side light value g4, so the thresholds in both directions
  meet with sensible hysteresis.
- `/dev/video99` shows what is displayed: it only reflects passthrough
  while passthrough is visible. That matches when switching matters, but
  translucent passthrough over an app would mix content in.

## 25. Calibration sweep (2026-09-30 22:54-22:59)

Room A light stepped through 100/60/35/20/12/7/4/2/1/off, 12 s per step,
first in colour, then in mono, by `hass.callService` from the Home
Assistant page (64 ms per call; HA's own last_changed matched my logged
call time within 0.11 s; PC and headset clocks agree within 0.1 s). No
IMUFallback. The user looked at a bed and a sloped wall this time, with
panels closed (a different scene from run 4).

Medians per step (first 4 s of each step skipped):

| light | colour: view mean | colour: g4 | mono: view mean | mono: grain (g8) |
| --- | --- | --- | --- | --- |
| 100% | 47.8 | 1.00 | 47.0 | 4 |
| 60% | 32 | 0.87 | 46.6 | 8 |
| 35% | 19 | 0.435 | 45.9 | 16 |
| 20% | 10.7 | 0.435 | 45.5 | 10-11 |
| 12% | 7.4 | 0.435 | 43.7 | 11 |
| 7% | 6.0 | 0.435 | 44.4 | 11 |
| 4% | 5.2 | 0.435 | 43.9 | 11 |
| 2% | 4.9 | 0.435 | 43.6 | 11 |
| 1% | 4.9 | 0.435 | 43.1 | 11 |
| off | 4.0 | 0.435 | 42.2 | 11 |

Findings:
- **Colour AE barely compensates**: the colour image darkens steadily
  (47.8 to 4.0). Colour passthrough is visibly poor from ~35% down.
- **g4 is not a linear light meter**: 1.0 at full light, 0.87 at 60%,
  then a floor of 0.435 from 35% down (in capture 1, fully dark, it went
  to 0.00). As a "colour is struggling" flag it works in every run:
  **g4 < ~0.6** when light <= 35%, >= 0.85 when light >= 60%.
- **Mono grain is non-monotonic**: 4 at 100%, 8 at 60%, peaks at 16 at 35%
  (visibly grainiest frame: maximum sensor gain), then 10-11 from 20% down
  where the image takes on the flat, hazy IR-illuminated look (XRService
  evidently leans on the IR illuminators and lowers gain). Every dim state
  is >= 10 and every bright one <= 8, so **grain <= ~7** means "light is
  back to at least ~60-100%".
- Mono view mean hardly moves (47 to 42), as expected with AE.

Resulting Phase 1 rule (to be validated in other rooms and views):
- colour to IR: g4 < 0.6 for the confirmation time;
- IR to colour: grain <= 7 for the confirmation time;
- the 35-60% band is a natural hysteresis gap: whichever mode is active
  stays.

Caveats: one room, two views; grain depends on scene texture (a busy
bookshelf would read higher than a plain wall); the estimator runs on
the composited view, so it is only meaningful while passthrough is
displayed (which is also the only time switching matters).

## 26. fapd first runs (observe-only), liveness bug, standby behaviour

Both streams carry a per-frame float64 timestamp at record+0x10 (records
at stream+0x38 + cam*0x1040 + rec*0x104). Only the displayed stream's
timestamps advance; a paused stream leaves its last value in place.

- **Bug found in the first observe-only run (23:12):** fapd treated the
  first timestamp it read as "advancing", so a stale value counted as a
  live frame and it began confirming a switch on stale data. Fixed with a
  baseline-first rule, now in `src/liveness.hpp` with tests (a mutant
  restoring the bug fails 22 checks).
- **Connecting as a `VRApplication_Overlay` client wakes the headset from
  standby.** 23:13:22: vrserver "leaving standby", displays on, tracking
  and passthrough cameras resumed, the instant fapd connected. Standby
  returned 5 s later (23:14:30) **while fapd was still connected**, so
  fapd does not keep the headset awake. Disconnecting an overlay client
  also causes one brief wake (23:15:25). A `VRApplication_Background`
  client (fapctl connect-test, 23:14:09) does not wake it.
  With autostart, fapd connects when SteamVR starts and disconnects when
  it quits, so neither wake happens in normal use.
- The second "passthrough in use" seen at 23:13:25 was real: our own
  connection had woken the headset.

## 27. Towards event-driven operation

User requirement: feel instant, stay lightweight, and do nothing at all
while passthrough is not in use (no 2 Hz idle polling).

OpenVR candidates (openvr.h): `VREvent_RoomViewShown` (526) /
`VREvent_RoomViewHidden` (527) ("for scene apps only - not for construct
or transient bounds"), `VREvent_EnterStandbyMode` / `LeaveStandbyMode`
(106/107), `VREvent_DashboardActivated/Deactivated` (502/503),
`VREvent_CameraSettingsHaveChanged` (851). Which of these fire on the
Frame when passthrough is toggled has to be measured.
`tools/event_probe` (Background client, does not wake the headset) logs
every event with its name.

### Measured (2026-09-30 23:18, event_probe, user toggling passthrough)

Passthrough toggled on/off three times (xrservice: resumed 23:18:25.6,
paused 30.6, resumed 35.5, paused 40.5, resumed 45.5, paused 50.5):

- **`VREvent_CameraSettingsHaveChanged` (851) fired on every toggle, on
  and off**, within ~50 ms, together with undocumented event 816.
- `VREvent_RoomViewShown/Hidden` (526/527) never fired, as the SDK
  comment warns.
- Opening the dashboard: `VREvent_DashboardActivated` (502).
- Taking the headset off: `VREvent_EnterStandbyMode` (106) at 23:19:12.9,
  the same moment xrservice logged onEnterStandby.

fapd is now event-driven: Idle (no reads at all; SteamVR event queue
checked once a second, mode file via inotify) -> Checking (after 851,
LeaveStandbyMode, a mode change or start-up; stream timestamps read at
4 Hz for up to 2 s) -> Active (passthrough frames arriving; 4 Hz loop,
grain at 2 Hz) -> Idle again on standby or when frames stop for 1.5 s
(with a grace period after fapd's own switch). OpenVR has no blocking
event wait, so the once-a-second queue check is the floor.

Measured idle cost (23:21, 20 s window, observe-only): 1 CPU tick
(10 ms, 0.05%), 20 voluntary wakeups (1/s), 1 thread, 16 MB RSS.

## 28. First live test with switching (2026-09-30 23:26-23:28)

fapd run manually with switching enabled, user wearing the headset with
passthrough on, room A light via HA:

- Light off 23:26:25 -> **switched to IR 23:26:29** (4 s). IR grain
  smoothed 12.75.
- Light on 23:26:54 -> **switched to colour 23:26:58** (4 s).
- No IMUFallback, no errors; compositor logged its usual
  `Late initialization successful` at each switch.
- Controller: holding the left thumbstick cycled auto -> colour -> IR ->
  auto as designed (23:27:06, :08, :13, :19). Going back to auto after a
  forced IR waited ~6 s for the anti-flicker dwell; fixed (manual switches
  no longer start the dwell), with a test.
- **False switch: at 23:27:47, with the light on, fapd switched to IR**
  (the user forced colour back at 23:28:11). After restart, colour light
  read ~0.63 at full light, versus 0.90-1.0 in the earlier views: g4
  depends on the scene, and 0.6 is too close. g4 is not reliable enough
  as the colour-to-IR trigger.
- Going to standby took fapd to idle immediately (23:28:27, 23:28:33).

## 29. XRService's IR emitter state is a better light signal

XRService logs `SLAMConsole: [IREmitters] IR Emitters Turned On/Off`
(mode `Auto` at session start). Lined up with every light change so far:

| test | light | emitters |
| --- | --- | --- |
| run 4 | off 22:40:48 | on 22:40:49.3 |
| run 4 | on 22:41:18 | off 22:41:24.2 |
| run 4 | off 22:41:38 | on 22:41:39.3 |
| run 4 | on 22:41:58 | off 22:42:04.2 |
| sweep, colour | 35% -> 20% at 22:55:07 | on 22:55:07.3 |
| sweep, colour | -> 100% at 22:56:31 | off 22:56:36.3 |
| sweep, mono | 35% -> 20% at 22:57:17 | on 22:57:17.3 |
| sweep, mono | -> 100% at 22:58:43 | off 22:58:48.2 |
| live test | off 23:26:25 | on 23:26:25.4 |
| live test | on 23:26:54 | off 23:26:59.2 |
| false switch | light on 23:27:47 | stayed off |

- Turns on within ~0-1.3 s of darkness, between 35% and 20% light (the
  same step in colour and mono mode), which is where colour passthrough
  becomes poor (colour view mean 19 at 35%, 10.7 at 20%).
- Turns off ~5-6 s after full light returns (XRService's own hysteresis).
- Independent of where the user looks (it is XRService's judgement for its
  tracking cameras), and it would not have made the 23:27:47 false switch.
- One unexplained on/off at 22:50:36-22:51:01 with the light on, probably
  the cameras briefly covered or facing somewhere dark.
- Source is a log line: event-driven via inotify on the log, but the
  format could change with a SteamVR update, so fapd must notice when the
  signal disappears and fall back.

## 30. Redesign: fast switching, adaptive cooldown, emitter-led evidence

User feedback: switching must feel instant, 10 s is too long, and it must
still never flicker; sampling should happen only when needed.

- LightPolicy now takes Evidence (Dark / Bright / Neutral / Unknown) and
  owns only timing: confirm_s 0.5, settle_s 1.5, stale_s 3, and an
  **adaptive cooldown** between automatic switches: 2 s, doubling for
  each switch within 60 s of the previous one, capped at 30 s, reset
  after a quiet minute. Manual switches never start it. Simulated light
  flickering every second for 2 minutes: 7 switches (1.5, 4.5, 9.5, 18.5,
  35.5, 66.5, 97.5 s) instead of ~120.
- Sensors (`src/sensors.*`):
  - colour shown: Dark when XRService's IR emitters are on AND the colour
    light value is below 0.6 (the colour value clears instantly when a
    hand leaves the cameras, while the emitters lag ~5 s; the emitters
    stop dim-looking views in good light from counting as dark);
  - IR shown: Bright when the emitters are off, or when mono grain is at
    or below 7 (fast path: grain drops within ~1 s, emitters take ~5 s);
  - fallbacks when the emitter line is missing: colour light < 0.5 ->
    Dark; grain <= 7 -> Bright.
- End-to-end simulation (measured sensor behaviour): lights off at 10 s ->
  IR at 11.5 s; lights on at 40 s -> colour at 41.0 s; 1.5 s hand over the
  cameras -> no switch; good light with changing views -> no switch.
- `src/emitter_watch.*` follows `~/.local/share/Steam/logs/xrservice.txt`
  (symlink to the session log) with inotify: reads the last emitter line
  once, then only appended bytes; follows XRService restarts. Tested
  against a fake logs directory, including a split line and a symlink
  rotation.
- fapd waits in poll() on the mode-file and emitter-log inotify handles.
  Wake intervals: idle 1 s (SteamVR event queue); active with nothing
  pending 1 s (liveness); while a decision could be pending 250 ms
  (500 ms with grain in IR). The view sampler runs only while IR is shown
  and the emitters are still on.
- Mutation checks: all 8 safeguards (colour check with emitters, grain
  fast path, unknown-vs-neutral, escalation, cooldown, reset, manual
  switches exempt, confirmation) are caught by the tests.

## 31. Walk-through test (2026-09-30 23:47-23:50): XRService crash, pitch-black false switch

The user walked through the house with fapd (section 30 build) running.

- 23:47:51 IR emitters on, colour light 0.01 -> switched to IR (correct).
- 23:48:39 switched to colour on "IR grain 5.3" (correct, lights on).
- 23:49:49 IR emitters on, colour light 0.00 -> switched to IR (correct).
- **23:49:55 switched to colour on "IR grain 2.3" while the user stood in
  pitch black.**
- Root cause chain, from vrserver.txt and coredumpctl:
  - XRService logged its last line at 23:49:49.269, then fell silent;
  - vrserver at 23:49:55.68: "No valid tracker state for 1.0 seconds",
    switched to 3DoF;
  - XRService **crashed with SIGSEGV** (core at 23:49:54, pid 2336) and
    was restarted by vrserver at 23:49:58 (new session log
    XRService-23-49-58.log).
- Crash stack (symbols recovered from nearby strings in the headset's own
  XRService binary): `WorldQueue::onFramePostTracked` ->
  `[SparsePipeline] runNextTask` -> local bundle adjustment ->
  `countRedundantAndTotalObservationForFrame` -> crash. That is SLAM
  mapping, not passthrough. It happened 5 s after the room went dark and
  the emitters came on; the same emitter-on + switch at 23:47:51 and ~12
  earlier switches did not crash. Best reading: a Valve SLAM bug in
  near-total darkness, not caused by fapd. Not proven; watch for repeats
  (`coredumpctl list`).
- fapd's wrong switch: with XRService stalled or the room pitch black,
  the IR view was blank or crushed to black, which has almost no grain
  (2.3), and the grain fast path read that as "bright".

Fixes:
- Grain only counts when the measured band is lit: mean >= 30 and at most
  20% near-black pixels (every IR frame in earlier tests: mean 44-89,
  <= 2% near-black). A dark frame also resets the grain smoothing.
- Grain only counts if the mono stream produced a new camera frame since
  the previous grain sample (a stalled XRService can leave a blank or
  frozen view).
- While SteamVR reports the HMD pose as not valid or not Running_OK,
  fapd holds (evidence Unknown) and logs it.
- Tests: pitch-black and blank-image cases, end-to-end "walk into a
  pitch-black room" replay (switches once, stays IR), black/grey frame
  stats; mutation removing the brightness gate fails 5 checks.

### Crash guard (added after section 31)

XRService crash history on this headset (`coredumpctl list`): SIGABRT on
2026-04-13 (before this project) and the SIGSEGV on 2026-09-30 23:49:54.
Rare, and tonight's is the only segfault, so a link to fapd's switch 5 s
earlier cannot be ruled out.

fapd now records every XRService restart that comes within 10 s of one of
its switches in `~/.config/frame-adaptive-passthrough/crash_guard`
(restart time taken from the new session log's name, so a restart during
an idle period is still timed correctly). With two entries fapd starts in
observe-only mode and says why; `fapctl guard` shows the entries and
`fapctl guard reset` clears them. Tonight's crash was entered by hand as
the first entry.

## 32. Decision: Phase 2 dropped (2026-09-30)

The user decided to drop the colourised-IR overlay as not worth it, and
the evidence supports that: passthrough is composited by vrcompositor with
fixed shaders and no blend control (section 5), so colourising would mean
rendering the full passthrough in our own process (stereo, reprojection,
latency, losing SteamVR's depth-mesh warp); IR reflectance does not map to
visible colour and a learned colour map goes stale for anything that
moves; and it would depend on the private frame-import interface every
frame, beside an XRService that crashed once during testing (section 31).
The project continues as Phase 1 only (fapd). The only built-in colour
control in the dark remains `camera.monochromeTintHue`, a single global
tint.

## 33. Lean rewrite: no SteamVR connection, no controller, systemd autostart

User direction (2026-10-01): purely automatic, no controller shortcuts, as
lightweight as possible.

Measured before the rewrite (fapd connected permanently as an overlay):
RSS 16 MB, but mostly shared libraries pulled in by SteamVR's client
library (vrclient.so 4.4 MB, libstdc++, libcrypto, libGL...); proportional
share (Pss) ~3 MB, private dirty 1.8 MB; fapd's own code 0.6 MB; 1 wakeup/s
while idle.

Facts behind the rewrite:
- XRService writes nothing to its log while the headset is idle (0 lines a
  minute over 20 minutes of standby) and ~1-4 lines a second in use, so
  following the log with inotify gives zero idle wakeups.
- Its log carries everything fapd needs: `[DeckardCaptureSource]
  Passthrough cameras resumed/paused` (every toggle, confirmed by the event
  test), `[UserPresence] Received onEnterStandby/onLeaveStandby`, the IR
  emitter lines, and `Transition to IMUFallback` / `[IMUFallback]
  Disabled` for visual tracking loss.
- A transient background connection (`fapctl get`) takes 10-18 ms and does
  not wake the headset.
- SteamVR runs as the systemd user unit `steamvr.service`; Valve's own
  `steamvr-v4l2cam.service` is a separate unit with
  `After=`/`Requires=steamvr.service`.

New design:
- fapd no longer links or connects to SteamVR. `src/xrservice_log.*`
  follows the session log (replacing the emitter watcher) and fapd sleeps
  in poll() on it. Active only while passthrough is shown and not in
  standby; while active it wakes only when a decision could be pending
  (250 ms), for grain in IR darkness (500 ms), or at 1 s when colour is
  shown with the emitters on; otherwise it sleeps until the log changes.
- Tracking loss now comes from the IMUFallback lines (fapd holds).
- Switching runs `fapctl set rgb|mono --quiet` (exit 0 ok, 3 interface
  changed, 4 camera not enabled); fapd disables switching on 3.
- Controller action, haptics, mode file and `fapctl mode` removed;
  `observe_only` in fapd.conf and `fapctl guard` remain for support.
- Autostart: `fapctl install` writes
  `~/.config/systemd/user/frame-adaptive-passthrough.service`
  (`After=` and `BindsTo=steamvr.service`, `WantedBy=steamvr.service`,
  Nice=10) and enables it; `fapctl uninstall` removes it and switches back
  to colour.
- Release builds: -Os, gc-sections, stripped: fapd 342 KB (was 8.5 MB),
  fapctl 550 KB.

### Deployed and installed (2026-10-01 00:08)

- `fapctl install` wrote the unit and the `steamvr.service.wants/` symlink;
  systemd started fapd (Memory 292K, peak 1.7M, CPU 4 ms at start).
- Bug fixed on deploy: the first loop pass slept before evaluating the
  initial state, so passthrough already shown at start-up would have gone
  unnoticed until XRService's next log line. The first pass now runs at
  once.
- Idle measurement, headset in standby (systemd-started fapd, 30 s): **0 CPU
  ticks, 0 voluntary wakeups; Pss 534 kB, Private_Dirty 220 kB, RSS 2 MB.**
- Not yet exercised with this build: a live switch, and SteamVR stopping
  and starting (the unit uses BindsTo=/WantedBy=steamvr.service). Pending
  the user's walk-through.

## 34. Walk-through 2 (2026-10-01 00:12-00:13): a close wall fools every IR-side signal

The user deliberately held the headset close to a wall in a dark room:

| time | fapd | XRService |
| --- | --- | --- |
| 00:12:15 | | IR emitters on |
| 00:12:16 | IR (emitters on, colour 0.00) | |
| 00:12:26 | colour: "IR grain 4.4, image mean 100" | |
| 00:12:31 | IR (cooldown safety net) | |
| 00:12:33-43 | | tracking lost (IMU fallback), too close |
| 00:12:48 | colour: "IR grain 5.5, image mean 106" | |
| 00:13:04 | IR (colour 0.30) | |
| 00:13:33 | | **IR emitters off, room still dark** |
| 00:13:34 | colour: "IR emitters off", **stuck** | |

- A surface close to the headset, lit by the emitters, gives a bright
  (mean 100+), low-grain IR image, and can make XRService itself turn its
  emitters off. Every signal from the IR side can be fooled this way; only
  the colour camera truly knows whether the room is lit, and it can only
  be read while colour is shown.
- It stayed on colour because the colour side required "emitters on" to
  call it dark.
- "Emitters on" has meant dark in every test (user's point); "emitters
  off" is the unreliable direction.

Changes (sensors.hpp):
- Grain removed entirely: while the emitters are on it is dark, whatever
  the IR image looks like. fapd no longer reads /dev/video99. Cost: IR to
  colour waits for XRService to turn the emitters off (~5 s after the
  light returns), so ~6 s instead of ~1-2 s.
- Colour shown, Dark also when g4 < 0.35 for 1.5 s regardless of the
  emitters.
- Verification: for 3 s after an automatic switch to colour, g4 < 0.35 is
  urgent Dark (confirm 0.25 s, skips the cooldown, still escalates it) and
  "emitters off" is distrusted until XRService turns the emitters on again.
- Settle after a switch to colour 0.3 s (colour's value is valid on its
  first frame); confirm 1 s for normal switches (a 1.5 s hand over the
  cameras was otherwise enough to switch once the dark colour value reads
  0.0, as it does in a truly dark room).
- Without the emitter signal nothing switches either way (no reliable
  way back from IR).

End-to-end replays: lights off -> IR at 2.0 s, on -> colour at 6.0 s; the
wall case -> one 0.75 s colour flash, then IR stays, also after leaving
the wall; emitters never on in the dark -> the same; hand for 1.5 s, view
changes in good light and 45% dimming -> no switch. Eight mutations of the
new safeguards are all caught. Also removed: the two room thumbnails in
tests/fixtures.

Deployed 00:22 (fapd 338 KB; Memory 320K per systemd; Pss 558 kB).

## 35. Walk-through 3 (2026-10-01 00:28-00:30): emitters stay on in bright rooms

Deployed build: emitter-only return to colour (section 34).

- 00:28:57 XRService turned its IR emitters on; fapd switched to IR at
  00:28:58 ("colour light 0.04"), correct.
- The user then walked into bright rooms. **The emitters stayed on until
  standby at 00:30:06** (70 s). fapd never returned to colour.
- The wall test caused no problem (the verification design held).
- So "emitters off" is not a dependable "light is back" signal either: in
  the room A the emitters went off ~5 s after the LIFX came to 100% every
  time, but ordinary room lighting did not make XRService turn them off.

Change: the grain path is back for IR to colour, with everything learned
since. Bright when grain <= 7 in a normally lit view (30 <= mean <= 90, at
most 20% near-black) held for 2 s, from a new camera frame; a colour check
after every automatic switch to colour; a grain-caused mistake locks grain
out for 60 s, doubling to 10 min. Emitters off still counts (fastest when
it happens). fapd samples the view (2 Hz) only while IR is shown and the
emitters are on (or "off" is distrusted), and logs "IR view: grain, mean,
near-black, emitters" every 10 s, so the next walk-through measures the
bright-room-emitters-on case directly (no data for it yet; thresholds come
from the room A sweeps).

End-to-end replays: off/on -> IR at 2.0 s, colour at 3.5 s; bright room
with emitters staying on -> colour at 3.5 s; dark with emitters on for
3 min -> no return; close wall -> one 0.75 s flash; medium-distance wall
for 3 min -> two 0.75 s flashes a minute apart. Six mutations of the grain
safeguards all caught. Deployed 00:34 (fapd 345 KB, 316K memory).

## 36. Faster return to colour (not yet deployed)

User: colour came back after walk-through 3's fix, "but took a bit".
Return latency was grain hold 2 s + confirmation 1 s (~3.5 s). Since every
automatic switch to colour is verified by the colour camera and undone in
under a second, that direction can be quick: grain_hold_s 0.5 and a
separate confirm_to_colour_s 0.25 (confirm_s stays 1 s towards IR, which
keeps a hand over the cameras from switching). Replays: lights on ->
colour 1.5 s after the light; close and medium walls unchanged (single
sub-second flashes). Built; deploy pending, headset switched off for the
night. The fapd.log "IR view" lines from that walk-through were not read
(headset off).

### Deployed 2026-10-01 13:28

The faster-return build is installed (Pss 516 kB). Last night's single
logged sample before the slow return (00:35:25: grain 5, mean 42, emitters
on) was within the "lit" rule, yet colour only returned at 00:35:33 via
"emitters off"; with one sample per 10 s logged, the most likely reading
is grain hovering around 7 and resetting the 2 s hold (now 0.5 s). Each
return to colour now logs the view samples of the previous 10 s so the
next slow return can be diagnosed from the log.

## 37. Walk-through 4 (2026-10-01 17:07): faster return works

User: "it all worked with no issues". Two dark/lit cycles:

| Time | Event |
| --- | --- |
| 17:07:45.6 | XRService: IR emitters on |
| 17:07:46 | fapd to IR (emitters on, colour 0.02) |
| 17:07:58 | fapd to colour by grain (6.6, mean 58); emitters stay on |
| 17:08:07 | fapd to IR (emitters on, colour 0.06) |
| 17:08:22 | fapd to colour by grain (3.6, mean 68) |
| 17:08:24.4 | XRService: IR emitters off |

The "IR view before the switch" samples show the light coming on cleanly:
grain 13-21 with mean 14-53 in the dark, dropping to 5-8 with mean 58-93
within half a second of the light (first cycle: 1.8 s before the switch;
second: about 1.5 s before). Notably the emitters never turned off during
the first lit period (about 9 s) and turned off 2.4 s after fapd's second
return, about 4 s after the light. So the grain path, not the emitters, is
what brings colour back, and ir_max_grain 7 separates these rooms well
(lit 3-8, dark 9-22 with the gates).

Emitter timing logging added (deployed 17:11): on each emitter change fapd
logs how long after the cameras first saw the room go dark (colour light
below colour_dark_light) or lit (colour light at or above colour_min_light,
or a usable IR view at or below ir_max_grain) it happened. Resolution is
the sampling rate: 1 s for the colour camera, 0.5 s for the IR view.

## 38. What turns XRService's IR emitters on and off (disassembly)

Read offline from XRService (`/opt/steamvr/drivers/cv/bin/linuxarm64/XRService`,
SteamVR 2.17.10) with capstone; nothing run on the headset. Addresses are
file virtual addresses in that build.

Auto mode is part of the tracking cameras' auto-exposure controller
(`PE::ActiveExposureController`), not a separate light sensor:

- Input: the median of a 256-bin luma histogram per tracking camera
  (ISP histogram when available). Ratio r = targetMedian / median
  (deadband 0.95..1.05). Desired gain Gd = r * current gain. Of the first
  two tracking cameras, the one needing the least correction wins
  (`0xda0480`). Analysis runs at most every 33 ms.
- State machine on exposure (`0xd9d2b0`): below 0.5 ms, up to 8.33 ms,
  8.33 ms, 16.66 ms (state 4, the longest exposure).
- On (`0xd9f498`): in state 4 with the emitters off and Gd > 1.2 * mid
  gain, IR pulse 0.5 ms, switched on at once. From a lit room this takes
  about three analyses (~100 ms): why "emitters on" is fast and reliable.
- While on (`0xd9fc80`), the pulse width (0.02..4 ms) follows the gain
  demand: the emitters dim themselves as the scene gets brighter.
- Off (`0xd9fcb8`): only after 5.0 s continuously in a low state (exposure
  below 8.33 ms, or 8.33 ms with Gd < 0.33 * (min + max gain)). Any return
  to state 4 or high gain resets the timer.
- Logged On/Off is the pulse ramp (`0xd9d7f0`) crossing zero; it also
  writes `led_state` 1/0 in the `scene_illum_ir_led` sysfs device.

Inferences that match the observations:
- Slow or no "off" in lit rooms (sections 35, 37): the emitters light the
  scene themselves, and in a dim-but-lit room the gain rarely stays low for
  5 s without interruption, so the timer keeps resetting.
- "Off" in the dark next to a close wall (section 33): the emitter-lit wall
  drives the median up, the state drops for 5 s, the emitters go off, and
  about 100 ms later darkness turns them back on.

Usable by fapd: only the on/off result is visible outside the process (the
log line we already follow, and `led_state`). Exposure, gain, the median
and the AE state are not published anywhere passive. The per-frame IR
pulse width is stored in each tracking frame's metadata (`0xf67b0c`, NaN
when off); not traced to shared memory (`VR_CameraPassthroughState` is not
created by XRService). If it did reach a passive reader, a pulse shrinking
towards 0.02 ms would be an early "the room is lit" signal, seconds before
the 5 s off timer.

Tunable only through `captureSessionSettings.trackingCameras.targetMedian`
(default 60) and `maxIrEmitterPulseWidth`, which would change tracking
exposure itself: not something to touch. The thresholds are hard-coded.

Conclusion: "emitters on" stays the trusted dark signal; "emitters off" is
late by design (5 s minimum) and can be prevented by the emitters' own
light, so the IR image check remains the main way back to colour.

## 39. Dusk false switch; the emitters become the judge of "dark"

2026-10-01 18:41-18:45, headset on the desk (face sensor covered), no
lamp (the LIFX was offline), daylight fading. The user: the room "never
went dark", yet fapd switched to IR and stayed there.

| Time | fapd |
| --- | --- |
| 18:41:58 | IR: colour light 0.14 for 1.5 s (emitters off) |
| 18:42:00 | colour: emitters off |
| 18:43:33 | IR: colour light 0.01 |
| 18:43:35 | colour: emitters off |
| 18:43:36 | IR: verify failed (0.01), "emitters off" distrusted |
| 18:43:55 | colour: IR grain 5.0, mean 34 |
| 18:43:56 | IR: verify failed (0.15), grain locked out 60 s |
| then | IR view grain 20, mean 48, emitters off: stuck in IR |

The colour camera reads 0.01-0.15 at dusk, as low as a dark room (the
00:13 wall case even read 0.30), while XRService's tracking cameras need
no emitters. With section 38's mechanism in mind (emitters on only at the
longest exposure with gain to spare), "emitters off" means the room has
usable light, and the IR view then has no advantage. The IR view without
emitters was also too noisy for the grain check, and the distrust only
ended when the emitters came on, so nothing could bring colour back.

Changes (sensors.hpp; user agreed):
- The "colour dark for 1.5 s whatever the emitters say" rule is gone.
  Colour to IR needs the emitters on (and colour below 0.6).
- Verification after an automatic switch to colour only fails if the
  emitters are on and colour is below 0.35.
- Distrust of "emitters off" now expires: 60 s, doubling per repeat up to
  600 s (`emitters_off_distrust_s`, `_max_s`), like the grain lockout.
  It used to last until the emitters came back on.
- `colour_dark_hold_s` removed.
- Accepted cost: the deliberate face-at-a-wall case in the dark (emitters
  fooled off) shows dark colour until the emitters come back on after
  leaving the wall (replay: 19 s at the wall, then IR).

Replays: dusk -> no switch; dark then dusk -> IR, then colour once the
emitters go off, and it stays; the earlier scenarios unchanged. Mutants
(no emitter gate, no distrust, no doubling) caught. Deployed 18:51.

First emitter timing line: 18:51:07 "IR emitters turned on 1.2 s after
the cameras first saw the room dark" (colour sampled at 1 Hz).

## 40. Cooldown only escalates for real flicker

Walk-through 18:55-18:57 (the dusk build): every switch was right, but
each took longer. The user toggled the lights by hand every 4-15 s, and
the old rule doubled the cooldown for any switch within 60 s of the
previous one: 18:56:06 emitters on -> IR only at 18:56:19 (cooldown 16 s),
18:56:39 emitters off -> colour at 18:56:49. The user: the cooldown is
only meant to stop strobing or flashing lights.

New rule (light_policy): a switch is flicker only if it comes within
`flicker_slack_s` (1.5 s) of the moment the cooldown allowed it; that
doubles the cooldown (cap 30 s). A slower switch steps it down one level,
and `cooldown_reset_s` (now 30 s) of quiet beyond the cooldown clears it.
Urgent reverts (a failed colour check) still escalate, as they come well
inside the cooldown. Test: lights toggled every 5 s for a minute -> all
12 switches within 1.25 s of the change, cooldown still 2 s; the 1 s
flicker test still backs off to 30 s gaps. The old rule fails the new
test. Deployed 18:59.

Emitter timing from the same walk-through (fapd's new log lines): on 0.9
to 1.2 s after the colour camera first read dark (1 s sampling, so
effectively immediate); off 0.4 to 0.6 s after the IR view first looked
lit (0.5 s sampling), much faster than the 5 s minimum suggests: the
5 s timer had evidently been running before the view crossed fapd's
grain threshold.

## 41. Unattended light tests (room B switch); the IR camera's light value

2026-10-01 21:43-21:58. Headset on a desk in the room B, face sensor
covered, passthrough shown; the room B ceiling light (Aqara H2 wall
switch, on/off only) toggled from Home Assistant (`hass.callService`). Light
times below are HA's `last_changed`. A passive recorder
(`tools/shm_records.py`) logged every new per-frame record of both
passthrough streams.

**Tracking drops in the dark.** 0.1-0.6 s after the light went out,
XRService entered IMU fallback (tracking lost), and with the headset
still it often stayed lost for the whole dark period and beyond (once from
21:51:28 until 21:57:45, through several lit periods). fapd held all
switching while tracking was lost, so it stayed on dark colour (21:43:51).
After allowing switches to IR it got stuck in IR with the light on instead
(21:51:43 onwards), since only the grain check could bring colour back and
the IR view is frozen while tracking is lost (identical grain/mean for
10 s in the "before the switch" lists).

**The IR camera's light value.** Each mono-stream record carries a float at
+0xEC, the same record offset as the colour stream's g4 (gamma marker
+0x14; mono records have 0.5 there instead of the 1/2.2 marker). It is:
- exactly 0.000 in the dark with the emitters on, for 70 s straight;
- 0.13-0.34 within 0.19-0.8 s of the light coming on (steps of 1/96),
  settling to ~0.22-0.30 as the emitters dim;
- unaffected by tracking loss (rose 0.45 s after the light while tracking
  stayed lost for another 3 s);
- only updated while IR is shown (like everything in the mono stream).
Not yet measured: a wall lit by the emitters from close by, and other
rooms.

Emitter timing (8 cycles): on 0.12-0.26 s after the light goes off; off
4.7-5.8 s after it comes on, every time (the 5 s timer of section 38).

Changes:
- `PassthroughSnapshot::mono_light`: the newest mono record's +0xEC.
- New IR-to-colour rule: IR light >= `ir_min_light` (0.1) for
  `ir_light_hold_s` (0.25 s), fresh frames only. A failed colour check
  after it locks it out together with grain (60 s, doubling).
- Tracking loss now only blocks the grain rule; going to IR, the IR light
  value and "emitters off" still work.
- The "IR view" log line includes the IR light value.

Results with the new build (21:55-21:58, 7 cycles, including 8 s on/off
toggling, tracking lost for most of it): every light-on returned to colour
within ~1 s ("IR camera light 0.30-0.33"), every light-off went to IR in
1.7-2.4 s. No wrong switches.

## 42. User walk-through: wall test with the IR light value

2026-10-01 22:03-22:04, user wearing the headset.
- 22:03:25 vrserver's XRServiceManager killed XRService "because it used
  too much memory" (SIGTERM, journal); SteamVR restarted it at 22:03:27.
  Not fapd-related (fapd's last switch was 344 s earlier; nothing of ours
  runs inside XRService); possibly map growth after many tracking losses
  in the unattended tests. The crash guard correctly did not count it.
- Wall test: at an IR-lit wall in the dark the IR light value read 0.10
  (exactly the threshold) for 0.25 s -> colour at 22:04:09, colour 0.24
  -> urgent revert to IR at 22:04:10, IR light locked out for 60 s. The
  user saw one brief black blink, then IR stayed.
- 22:04:33 emitters went off (18 s after the first lit reading); colour
  at 22:04:34, not reverted (the user was presumably away from the wall).

Change: `ir_min_light` 0.10 -> 0.15. Lit rooms read 0.22-0.34 (first
frame after the light 0.13-0.19, so at most one extra frame of delay); the
wall read 0.10. Deployed 22:06.

## 43. The image check is gone

User question: is the image (grain) check still needed? No. Since section
41 every return to colour came from the IR light value (~1 s after the
light), never from grain, and grain was the heaviest and most fragile part
of fapd: a 2 Hz V4L2 capture of /dev/video99, frozen while tracking is lost,
fooled by surfaces lit by the emitters.

Removed: src/view_grain.*, its tests, the grain rules and config keys
(`ir_max_grain`, `ir_min_mean`, `ir_max_mean`, `ir_max_dark_fraction`,
`grain_hold_s`; `grain_lockout_s`/`_max_s` became `ir_light_lockout_s`/
`_max_s`). The tracking-loss special case went with it: no remaining signal
depends on tracking. fapd now only reads shared memory and XRService's log;
in IR it polls the IR light value at 2 Hz (a 25 KB tmpfs read; SteamVR
writes the segment without any notification, so it cannot be event-driven)
and logs it every 10 s; returns to colour log the last 10 s of readings.
The diagnostic probes that opened camera devices during discovery are not part of the published repository.

Known gap: a lit room whose IR light value stays below 0.15 (a dim lamp,
not measured) returns to colour only when the emitters go off, ~5 s after
the light instead of ~1 s.

Measured after deploying (22:14): fapd 342 KB, Pss 490 kB, no video device
open.

## 44. Walk-through: wall at touching distance (corrected with HA history)

2026-10-01 22:18-22:19, user wearing the headset in the room B, face at
and briefly touching a wall; the user then turned the ceiling light on and
off. Home Assistant history (`/api/history/period`) gives the light times,
local:

| Time | Event |
| --- | --- |
| 22:18:28-29 | emitters on, fapd to IR (colour 0.01) |
| 22:18:56.1 | room B light on |
| 22:18:57-22:19:01 | IR light value 0.03 then 0.00 (facing the wall) |
| 22:19:02 | emitters off (5.9 s after the light); fapd to colour: correct |
| 22:19:04.9 | room B light off |
| 22:19:05-06 | emitters on; fapd to IR (colour 0.09): correct |
| 22:19:16 | fapd to colour on IR light 0.23-0.25; the room B light was still off, so another light source (probably the user leaving the room) |

I first read 22:19:02 as the wall switching the emitters off in the dark,
added a 5 s hold on "emitters off" (19a5176) and deployed it. The HA
timeline refutes that, so it was reverted (deployed again 22:23): the hold
fixed nothing observed and would add 5 s to the return whenever the IR light
value cannot see the room light.

New fact: **facing a wall from centimetres away, the IR light value reads
about 0 even with the room light on** (0.00-0.03 at 22:18:57-22:19:01, while
lit rooms read 0.22-0.34). The emitters lighting the wall dominate what the
IR camera sees. In that case only "emitters off" (~5 s after the light)
brings colour back; this is why "emitters off" must stay a prompt way back.

Lesson: check the light's actual timeline (HA history) before explaining a
switch; an explanation from fapd's log alone was wrong here.

## 45. Faster switching, measured to the frame

2026-10-01 22:27-22:42, headset on a desk in the room B, ceiling light
toggled through Home Assistant; switch times taken from the passthrough
config byte in shared memory (`tools/shm_records.py` at 100 Hz), light times
from HA history, emitter times from XRService's log. Seconds after the
light change:

| | emitters | signal | fapd switched |
| --- | --- | --- | --- |
| off, old build (4 cycles) | on 0.18-0.21 | colour g4 < 0.6 at 0.69-0.71 | 2.22-2.27 |
| on, old build (4) | off 5.27-5.31 | IR light >= 0.15 at 0.35-0.40 | 1.15-1.59 |
| off, new build (7) | on 0.19-0.25 | colour g4 < 0.6 at 0.64-0.73 | 1.33-1.63 |
| on, new build (7) | off 5.22-5.47 | IR light >= 0.15 at 0.31-0.56 | 0.67-0.94 |

Where the time went: the colour camera's g4 is already smoothed by the
camera (0.89 to 0.44 over ~1.5 s after the light goes out), and fapd's own
0.5 s EMA on top roughly doubled the lag; then 1 s of confirmation. The IR
light value steps 0 -> 0.33 within ~0.1 s, but was polled at 2 Hz and held
0.25 s on top of the 0.25 s confirmation.

Changes: `smoothing_s` 0.5 -> 0 (no extra smoothing), `confirm_s` 1.0 ->
0.5 (switching to IR also needs the emitters on, which a hand over the
colour camera does not cause), `ir_light_hold_s` 0.25 -> 0, IR light polled
at 4 Hz while it matters. All 14 switches correct, no flicker.

What is left before IR: the colour value needs ~0.7 s to fall below 0.6
(the camera's own smoothing), plus 0.5 s confirmation. Emitters are on at
~0.2 s, but they can also turn on in a lit room (a hand or a dark spot,
2026-10-01 18:11:09), so they are not enough on their own.

Testing note: the Home Assistant page suspends its websocket while its tab
is hidden (`callService` rejects with code 3); the REST API from the same
page session (`fetch('/api/services/light/turn_on')` with the session's
access token) works regardless.

## 46. IR light value at a wall in the dark; threshold stays 0.15

2026-10-01 22:46 manual test: one slow return to colour (22:46:31, via
"emitters off"): the IR light value read only 0.12 in that lit spot, under
the 0.15 threshold. Other returns read 0.17-0.19 (dimmer than the room B
ceiling light's 0.3).

22:49:30-22:50:08, user staring at a wall with the IR emitters on in the
dark (room B light off per HA), sampled every frame (20 Hz): almost all
0.00, peaks 0.07 and 0.13 (22:49:49), plus 0.19 on the first frames right
after the switch to IR (covered by settle_s). 22:50:00 (0.30) was the user
walking back into the lit room A.

So a wall in the dark reaches 0.13 and a dim lit room can read 0.12: the
value alone cannot separate them in that band. Decision (user): keep 0.15;
dim rooms fall back to "emitters off" (~5 s), which is acceptable since
colour is the direction you can still see in.

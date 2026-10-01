# frame-autopass

> **AI disclaimer:** built with the help of AI and tested on a single
> headset. Treat it as experimental.

Automatic passthrough switching for the **Steam Frame with the Arcturus
Vision colour module**: colour passthrough in good light, the Frame's own
infrared (IR) passthrough in the dark. Turn the lights off and you can see
in IR about 1.5 s later; turn them on and colour is back in about a second.
There is nothing to press and nothing to configure.

It runs entirely on the headset, never talks to the network, and does
nothing at all while passthrough is not showing.

## Requirements

- A Steam Frame with the Arcturus Vision colour passthrough module attached.
- Tested on SteamOS 0.3.0 with SteamVR 2.17.10. Other versions may work; if
  SteamVR changes the interface it uses, it stops switching safely (see
  [After a SteamVR update](#after-a-steamvr-update)).
- Desktop Mode with a terminal (Konsole). Developer mode was enabled on the
  test headset; it may not be required.

## Install

On the headset, switch to Desktop Mode, copy the command below (copy button
on the right of the box), open **Konsole** and click **Paste** at the top
right, then press Enter:

```sh
curl -fsSL https://github.com/bod09/frame-autopass/releases/latest/download/install.sh | bash
```

Or the short version, easier to type in VR:

```sh
curl -L bod09.github.io/frame-autopass/i | bash
```

That's it. It starts automatically whenever SteamVR runs, from now on.

The installer checks that the Arcturus module is present, downloads the
latest release, verifies its checksum, puts two programs in
`~/.local/share/frame-autopass` and registers a systemd user service that
starts and stops with SteamVR. Everything stays in your home folder, so it
survives SteamOS updates and needs no root access.

Prefer not to pipe a script into bash? Download
`frame-autopass-aarch64.tar.gz` from the
[releases page](https://github.com/bod09/frame-autopass/releases), unpack
it, read `install.sh`, and run `./install.sh` from that folder.

## Use

Just use passthrough as normal. The commands below are optional (run them
in Konsole):

```sh
~/.local/share/frame-autopass/autopass status      # running? what is it showing, and why
~/.local/share/frame-autopass/autopass update      # install the latest release
~/.local/share/frame-autopass/autopass report      # write ~/frame-autopass-report.txt for a bug report
~/.local/share/frame-autopass/autopass uninstall   # remove the service, back to plain colour passthrough
```

`uninstall` stops the service and removes it; delete
`~/.local/share/frame-autopass` and `~/.config/frame-autopass` as well to
remove every trace.

## How it decides

It reads three signals, none of which needs an image or a camera device:

- **XRService's IR emitters.** Valve's tracking service turns the IR
  emitters on when its tracking cameras run out of light (about 0.2 s after
  the room goes dark) and off once there is light again (at least 5 s
  later). It says so in its log, which this follows.
- **The colour camera's light value**, published by SteamVR for every
  colour frame.
- **The IR camera's light value**, published for every IR frame: 0 in the
  dark even with the emitters on, about 0.2 to 0.35 in a lit room.

From these:

- **To IR:** the emitters are on and the colour camera reads dim. The
  emitters decide what "dark" is, so a dim room at dusk stays in colour.
- **To colour:** the IR camera sees light, or the emitters have turned off.
- **Mistakes are undone fast:** for 3 s after switching to colour, if the
  emitters are on and the colour camera sees darkness, it goes straight back
  to IR and ignores whatever misled it for a minute (doubling if it repeats).
  At worst you see one sub-second blink.
- **No flicker:** after a switch it waits at least 2 s before switching
  back; that wait only grows (up to 30 s) if the light keeps changing back
  the moment a switch is allowed, as a flashing light would.
- Losing tracking (which happens in the dark if you stand still) does not
  stop it working.

Known limits: in a dimly lit room, or with your face right up against a
wall, the IR camera may not see enough light, so colour comes back when the
emitters turn off, about 5 s after the light instead of 1 s. It never
leaves you in the dark for that: the slow direction is always back to
colour.

The research behind every rule, with measurements, is in
[FINDINGS.md](FINDINGS.md).

## After a SteamVR update

There is no public way to switch the passthrough camera, so frame-autopass
uses a private SteamVR interface. Before every switch it checks that
SteamVR's code still matches what it was built against. If an update
changes it, frame-autopass stops switching (passthrough keeps working
normally, just without automatic switching) and `autopass status` says so.
Run `autopass update`; if no fixed release exists yet, please open an issue
with the output of `autopass report`.

## Safety

- It never opens a camera, never writes outside your home folder, and does
  not connect to the internet (except `update`, when you run it).
- It only reads SteamVR's shared memory and logs. To switch, it connects to
  SteamVR for about 15 ms as a background client, which never wakes the
  headset or starts SteamVR.
- Crash guard: if XRService (which also runs tracking) restarts within 10 s
  of a switch twice, switching pauses until you run `autopass guard reset`.
  This exists because XRService once crashed during early testing; the
  crash was most likely unrelated, but switching should not continue if it
  ever repeats.
- Measured cost on the headset: about 0.5 MB of memory, no CPU or wake-ups
  while passthrough is hidden.

## Tuning

Optional: put `key = value` lines in `~/.config/frame-autopass/autopass.conf`
and restart SteamVR. The keys and defaults are listed at the top of
[src/daemon_config.hpp](src/daemon_config.hpp). Unknown keys are rejected
and logged, so a typo never silently does nothing.

## Building from source

On an x86_64 Linux PC:

```sh
scripts/fetch-deps.sh   # Zig toolchain and OpenVR SDK headers, into gitignored folders
make test               # host tests
make dist               # cross-compiled release package in dist/
```

`make deploy` copies a build to a headset reachable as `ssh frame`.
GitHub Actions builds and tests every push and publishes a release for every
`v*` tag.

`tools/` holds the research tools used to find all of this: a passive
shared-memory recorder and sampler, and an annotated ARM64 disassembler for
SteamVR's binaries.

## Credits

The existence and name of SteamVR's private passthrough camera interface
were first learned from
[KominoVR/frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts),
which toggles the camera with controller gestures. No code from it is used;
everything here was re-derived from the headset's own binaries.

MIT licence, see [LICENSE](LICENSE).

# Dryer Link

Firmware that connects a GE clothes dryer to Home Assistant, for dryers it has not seen before.
"Dryer Link" is a placeholder name.

It runs on the OnionStraws Rev 2.2 "mod1" adapter (the open-hardware mulcmu board with an ESP32-C3-WROOM-02), which
plugs into the RJ45 service port on the back of the dryer and is powered from it.

**Status, 2026-11-22: version 0.1.0 compiles and passes its tests on a computer. It has not yet run on a module.**
See "What is tested" below.

## What it does

1. On first power-up the module opens its own Wi-Fi network, named like the module: `dryer-link-xxxxxx`. The
   password is on the module's label, with a QR code that joins the network.
2. Joining that network with a phone opens a walkthrough. It:
   - finds the dryer on either of GE's two buses (the older GEA2 or the newer GEA3),
   - reads what kind of appliance it is, and stops politely if it is not a dryer,
   - learns which data points this dryer has: a quick check of about 2 minutes, or a full scan of about 12,
   - offers two hands-on checks, each skippable: open and close the door, press Power,
   - joins the home Wi-Fi last, because that closes the setup network and with it the page.
3. Entities for things this dryer does not have are hidden, and those data points are never polled.
4. Home Assistant finds the module on its own and assigns its own encryption key when it is added.
   Without Home Assistant, the module serves a status page at `http://dryer-link-xxxxxx.local`
   (user `admin`, password from the label).
5. The module keeps itself up to date: it fetches new releases of this repository by itself.

It never sends a start command. What it can write is listed under "What it writes to the dryer".

## Things an installer should know

- **The label.** `flash.sh` gives every module its own password and writes a label to print. The password locks the
  setup network and the status page. It is stored on the module and in the label file, nowhere else. If a label is
  lost, flash the module again through the adapter: it gets a new one.
- **Setup window.** The setup network closes 30 minutes after power-up if the module has not been set up. Home
  Assistant can add the module only during the first 30 minutes after a start. In both cases: unplug the module's
  cable for ten seconds and plug it back in.
- **The end of the walkthrough.** When the module joins the home Wi-Fi it closes its setup network, so the page
  says "That should be it" and lists what to do next. The green light on the module is on once it has joined. If
  the setup network shows up again, the Wi-Fi password was wrong.
- **A restart after setup is normal.** A few seconds after joining Wi-Fi the module restarts once, to apply what it
  learned (ESPHome only allows entities to be hidden while a device starts).
- **Updates install themselves** when the dryer is not running, paused or waiting on a delayed start. The module
  restarts once for each.
- **Full reset.** Unplug and replug the module five times, each within ten seconds of the last. It forgets its Wi-Fi
  and what it learned. It keeps its password.
- **Moved to another dryer?** The module notices the different model, learns it again on its own and restarts once.

## Security

- The firmware is the same on every module and is public, so it contains no passwords and no keys.
- **Setup network:** WPA2, with the module's own password (12 random characters). The home Wi-Fi password is typed
  in over that network.
- **On the home network** the release firmware serves the status page behind the label password, Home Assistant's
  encrypted connection, and nothing else. The walkthrough and its commands are not reachable there. Nothing can
  push firmware to a module.
- **Updates** are fetched by the module over HTTPS with the certificate checked, and the download is compared with
  the checksum in the release's manifest. If a new version does not start, the bootloader returns to the previous
  one. The firmware is not signed: whoever can publish to this repository's GitHub Pages can update every module.
  Protect the GitHub account accordingly (two-factor sign-in, no shared access).
- **Home Assistant adoption** is ESPHome's: for 30 minutes after a start, the first Home Assistant (or anyone else
  on the same network who speaks the protocol) to connect sets the key. After that only the key holder gets in.
- A module flashed without `flash.sh` has no password: open setup network, no login on the status page.
- The **test build** (`dryer-link-test.yaml`) accepts firmware from anyone on the same network, has no login, and
  answers the walkthrough's commands on the home network. It must not be installed for a customer.

## What it writes to the dryer

- **Stop**, when the owner presses it (0x2040 = 0), if the dryer has that data point.
- **Extended Tumble**, when the owner switches it (0x201B, or 0x2051 on newer dryers), if the dryer has it.
- **While learning, once per option:** the value Extended Tumble already has is read and written straight back,
  unchanged. A dryer that does not acknowledge that write does not accept writes, and the switch is left out
  instead of being offered and doing nothing. (A GFD48GSSK0WW answers reads of 0x201B but has been documented as
  read-only.) Stop cannot be checked that way, because writing it would stop a cycle.

Finding the dryer and the two hands-on checks only read. No start command exists in the firmware; on the
GTD65GBSJ3WS every start command is acknowledged and ignored.

## Files

| Path | What it is |
|------|------------|
| `dryer-link.yaml` | The device configuration: pins, entities, the list of features a dryer may have, the version. |
| `dryer-link-test.yaml` | Test build: the same plus network upload, no login, debug logging. Not for customers. |
| `components/dryer_link/` | The component that finds and learns the dryer, serves the walkthrough and installs updates. |
| `components/dryer_link/setup.html` | The walkthrough page. Opened as a plain file it runs against a pretend dryer. |
| `components/gea/` | The bus component, vendored from mguaylam/esphome-gea with additions (see its `CHANGES-dryer-link.md`). |
| `flash.sh`, `tools/flash_module.py` | Flash a module through the USB-serial adapter and make its label. |
| `tools/release.py` | Checks a built firmware, writes the update manifest, and checks the update address. |
| `tools/gen_scan_table.py`, `tools/check_features.py` | Regenerate the list of codes the scan asks for; check the tests use the firmware's feature list. |
| `tests/host/` | Runs the real components on a computer against pretend dryers. |
| `tests/tools/` | Tests the flash tool and the label format against the firmware's own parser. |
| `.github/workflows/build.yml` | Tests on every push; builds and publishes a release when started by hand. |
| `DESIGN.md` | Decisions, how it works, what is known about other GE dryers, and what is not done. |

## Build and flash

Needs ESPHome 2026.10.2. No `secrets.yaml` is used.

```sh
./flash.sh            # release firmware: wipes the module, flashes it, gives it a password, writes its label
./flash.sh --test     # the same with the test build
./flash.sh --keep     # firmware only: the module keeps its Wi-Fi details, what it learned, and its password
```

The board has no USB port. Flashing goes through the J2 header with a 3.3 V USB-serial adapter (3v3, GND, TX to TX,
RX to RX, hold SW2 while plugging in). Labels are written to `labels/` as SVG files, 62 x 29 mm; that folder holds
the passwords and is not part of the repository.

The test build can afterwards be updated over Wi-Fi:

```sh
esphome upload dryer-link-test.yaml --device dryer-link-xxxxxx.local
```

## Publishing a release

1. Raise `version:` in `dryer-link.yaml` and push to `main`.
2. Try that firmware on a module of your own.
3. On GitHub: Actions, "Build", "Run workflow", type the version, run.
4. When the run reaches "publish" it waits: open it, "Review deployments", tick `github-pages`, approve.

The workflow runs the tests, builds the firmware, puts it at the update address
(`https://jdbressler.github.io/dryer-link/firmware/manifest.json`, GitHub Pages), fetches it back the way a module
does, and makes a GitHub release. The approval in step 4 is there because a push alone must never be able to
publish: the repository's `github-pages` environment only deploys after the account owner approves. Modules check the address shortly after start-up and every 4 hours, and
install a version that differs from their own when the dryer is quiet. A release that fails to install is retried
after 5 minutes, then 20, then 80, and so on up to once a day.

**The update address is built into every module.** If the repository or the account is renamed, modules in the
field stop finding updates. Before many modules are out, consider serving the manifest from a domain of your own.

## Adding something a dryer may have

Add the entity to `dryer-link.yaml` with an `id`, then add a line to `dryer_link: features:` with the data point's
code, the words the walkthrough should use for it, and that `id`. A dryer that does not answer for the code will not
show the entity. For an option the owner can switch, add `write_check:` with the code a change is written to.
Copy the line into `FEATURES` in `tests/host/sim.cpp` too; `tools/check_features.py` fails the build if they differ.

## What is tested

On a computer, on every push:

- `tests/host/build.sh`, then `build/sim --test`: the real bus component and the real `dryer_link` component,
  compiled for the computer, against pretend dryers. 374 checks: a dryer like the GTD65GBSJ3WS, a washer, a dryer
  that does not report its type, two boards on one bus, a dryer on the newer bus, the cable coming out during
  learning, a collision on the wire, flash that cannot be written, a module moved to a different dryer, a dryer like
  the GFD48GSSK0WW, an older dryer as GE documents them, a board that ignores broadcasts, newer-bus appliances that
  answer only at their own address, refuse a subscription or answer "busy", the label password (valid and damaged),
  what is served on the setup network and what on the home network, and self-update with its waiting rules.
- `tests/tools/test_flash_module.py`: the label the flash tool writes is read back by the firmware's own parser,
  and the commands it gives the flashing program.

By hand (`node tests/host/walk.js`, needs Playwright): the real walkthrough page in a phone-sized browser, clicked
through from the first screen to the last, including a wrong Wi-Fi password and a page that is closed and reopened.

Not tested anywhere yet:

- Anything on a real module: the setup network and its password, how phones open the page, the join to Wi-Fi, the
  status-page login, Home Assistant adoption, an update fetched from GitHub.
- A real dryer's timing. The pretend dryers use the frame format captured from a GTD65GBSJ3WS, but they are pretend.
- The newer bus (GEA3) against a real appliance. Nobody involved has such a dryer.
- Any GE dryer other than a GTD65GBSJ3WS. What is known about others comes from public sources (see DESIGN.md).

## Credits

- Bus component: [mguaylam/esphome-gea](https://github.com/mguaylam/esphome-gea), MIT, (c) 2024 mguaylam.
- Board: [mulcmu/esphome-ge-laundry-uart](https://github.com/mulcmu/esphome-ge-laundry-uart), MIT, (c) 2021 mulcmu.
- Data-point numbers and value tables: [GEAppliances/erd-definitions](https://github.com/GEAppliances/erd-definitions),
  BSD 3-Clause, (c) 2024 GE Appliances.
- QR codes on the labels: [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library), MIT,
  (c) Project Nayuki (`tools/qrcodegen.py`).

Dryer Link's own code is under the MIT licence (`LICENSE`), (c) 2026 Snow Wood Tech.
This project is not affiliated with or endorsed by GE Appliances.

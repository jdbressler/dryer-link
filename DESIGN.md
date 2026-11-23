# Dryer Link: design record

Status 2026-11-22: firmware 0.1.0 written. It compiles with ESPHome 2026.10.2 and passes its tests on a computer.
It has not yet run on a module or against a real dryer. "Dryer Link" is a placeholder name.

## Decisions (made by the owner)
- Cover both of GE's bus languages on the OnionStraws Rev 2.2 mod1 board: the older one (GEA2) and the newer one (GEA3).
- First boot opens the module's own hotspot. What appears is the walkthrough, not ESPHome's stock Wi-Fi page.
- Learning defaults to a quick check; a full scan is offered with its duration stated.
- Two hands-on checks, each skippable: open and close the door, press Power.
- Sites may have no Home Assistant: the module also serves a status page. Home Assistant assigns its own key.
- Source in a public GitHub repository (`jdbressler/dryer-link`, MIT, (c) Snow Wood Tech); modules update themselves
  from its releases.
- Secure and easy to use.

## How it works

### Finding the dryer
The module tries, in turn, until something answers:
1. Classic bus (GEA2, 19200 baud, one wire, GPIO5/GPIO10 both inverted): broadcast a read of the appliance type
   (0x0008). GE's definition allows only the board that hosts the public data to answer, so on an appliance with
   several boards this picks the right one.
2. Classic bus again: broadcast a read of the model number (0x0001), which every board has.
3. New bus (GEA3, 230400 baud, GPIO21 out / GPIO20 in): subscribe at 0xC0, where GE puts the main board on every
   GEA3 appliance and where GE's own adapter subscribes. Every other round the subscription goes to the broadcast
   address instead. The module's own address on this bus is 0xE4, the one GE's adapter uses.
4. Classic bus, address by address: ask 0x24, 0x2B, the rest of 0x20-0x2F and 0xC0 directly for the model number,
   in case a board does not answer broadcasts. 0x24 is the address GE documents for its older dryers and the one
   a GFD48GSSK0WW answers at; a GTD65GBSJ3WS is at 0x2B.

A full round takes about 10 seconds. After two rounds with no answer the page says "I can't hear the dryer yet",
and the search continues, so plugging the cable in is enough. Two boards that cannot be told apart are reported, not
guessed.

Then the identity is read: appliance type, model, serial. Type 1 (clothes dryer), 23 (washer-dryer combination) and
46 (laundry center) are accepted. A known other type ends at "This isn't a dryer I know". A type that cannot be read
is accepted if the laundry machine state (0x2000) answers; older dryers and the GFD48GSSK0WW have no type to read.

### Learning
The firmware has a list of features (`dryer_link: features:` in `dryer-link.yaml`): a data point, the words the page
uses for it, and the entities that depend on it.

- Quick check, about 2 minutes: the feature data points, asked up to three times each, then GE's common block
  (0x0000-0x0FFF) and laundry block (0x2000-0x2FFF) of known codes, 517 codes in all, asked once each.
- Full scan, about 10 to 12 minutes on the classic bus: every one of the 2,449 known codes.
- On the new bus the dryer publishes everything it has after the subscription; those count as answered and are not
  asked again. An appliance that refuses the subscription (GE: some allow only one subscriber) is asked code by
  code, and afterwards its entity data points are polled, with a new subscription attempt every 30 seconds.
- "Busy" (a GEA3 answer) is not "I don't have that": the question is repeated up to five times.

Most codes go unanswered by design, so silence proves nothing. Every 100 questions, and before the result is
accepted, the module asks for the model number, which this dryer is known to answer. If that fails, learning stops
without saving and the search starts again. A single-attempt question lost to a collision on the wire is asked once
more.

Last, for each option the owner could switch (`write_check:` in the feature list): the option's value is read
again and written straight back, unchanged. If the dryer does not acknowledge the write, the option is treated as
absent. Reason: a GFD48GSSK0WW answers a read of 0x201B but is documented as accepting no writes, and a switch that
does nothing is worse than no switch. On GEA3 the write's result code decides (0 accepted, 1 no such data point).
On GEA2 there is no result code, only an answer or silence.

What is kept in flash: which features are present, a hash of the model, which bus, and the count of answered codes
no feature uses. The raw values of everything that answered stay in memory for the report (`/dl/report`) until the
next restart.

### Adapting
- A feature whose data point did not answer: its entities are hidden and it is never polled. (An unanswered poll
  holds the classic bus for almost three seconds.)
- ESPHome allows hiding only while the device starts. So the module saves the result and restarts once, a few seconds
  after it gets onto Wi-Fi. During first setup that is after the walkthrough has finished.
- A learned module that finds a different model, or the same dryer on the other bus, learns again on its own (quick
  check) and restarts once.
- A module that was never learned and has no hotspot up (nobody can be at the page) does the quick check on its own.
- Value tables come from GE's public definitions plus values seen on a GTD65GBSJ3WS. A value that is not listed shows
  as its number.
- A dryer that has both forms of Extended Tumble (0x201B and 0x2053) shows two switches for the same option in Home
  Assistant. GE says newer dryers keep 0x201B in step with 0x2053. Left as it is until seen on a real dryer.

### Checks
- Door: 0x2012 is read every 0.4 s. Open seen, then closed seen, passes. Nothing within 45 s is "I didn't see the door
  move". If the owner then carries on without it, the door entity is hidden: a door that never reports would
  otherwise read "closed" forever. Skipping the check hides nothing.
- Power: 0x2000 is read every 0.4 s; any change from its first value passes.

## What the module serves
On the hotspot every address returns the walkthrough (that is what makes phones open it). The page's API:

- `GET /dl/status` JSON: `name`, `fw`, `hotspot`, `secured` (the module has a label password), `login` (the status
  page asks for it), `bus {state: searching|found|none|unsupported, lang: classic|new, address}`,
  `dryer {type, model, serial}`, `learn {state: idle|running|done, mode: quick|full, done, total, secs_left, can[],
  controls[], skipped, unknown}`, `check {what: none|door|power, phase: idle|wait1|wait2|ok|timeout}`,
  `caps {door, power}`, `live {state, door}`, `wifi {state: idle|connecting|ok|fail, ssid}`
- `GET /dl/find`, `GET /dl/learn?mode=quick|full`, `GET /dl/check?what=door|power|none`
- `GET /dl/report` plain text for support
- `GET /wifisave?ssid=&psk=` and ESPHome's own `GET /config.json` (network list)

All of that exists only while the hotspot is up. On the home network the release build serves ESPHome's status page
and nothing of its own. The test build (`setup_on_network: true`) also serves `/setup` and `/dl/...` there.

When the module joins Wi-Fi, ESPHome shuts the hotspot at once, so the page never sees "joined". The page treats
five failed requests in a row while joining as the normal ending ("That should be it"), says how to confirm it, and
goes back to "That didn't work" if the hotspot answers again with a failed join.

The web server runs on its own task. Everything it shares with the main loop sits behind one lock; requests from the
page only set state, and the main loop does the bus and flash work.

## Security

What is protected, and by what:

| Surface | Release build | Protection |
|---------|---------------|------------|
| Setup hotspot | up until the module is on Wi-Fi, at most 30 minutes after a start | WPA2 with the module's own password |
| Walkthrough and `/dl/` API | hotspot only | the hotspot's password |
| Status page (port 80) | home network | digest login, user `admin`, the module's own password |
| Home Assistant API (port 6053) | home network | ESPHome: the first client within 30 minutes of a start sets the key; encrypted afterwards |
| Firmware over the network | not possible to push | the module fetches from one HTTPS address, checks the certificate and the checksum |
| Serial header J2 | physical access | none; whoever can open the module owns it |

- **The module's own password.** `tools/flash_module.py` makes 12 random characters (31-letter alphabet without
  look-alikes, about 59 bits), writes them to a 4 kB flash partition of their own (`dl_label`, type 0x40, at
  0x3E0000) and to a label file. Layout: `DLK1`, one length byte, the password, CRC-16 (polynomial 0x1021, start
  0xFFFF). The firmware reads it at start-up, before Wi-Fi starts, sets it as the hotspot's password, and replaces
  the placeholder login of the status page with it. Firmware updates and the five-restart reset do not touch that
  partition. A partition that does not parse counts as "no password": open hotspot, no login.
- **No secrets in the firmware.** The configuration's `web_server: auth:` carries a placeholder only to compile the
  login in; it never applies, because the firmware overwrites or clears it before the network is up.
- **The password is never served** by the status JSON or the report. ESPHome logs the hotspot's password once when
  the hotspot starts; the log is reachable only through the status page (behind that password) and Home Assistant.
- **Updates are not signed.** Trust is the TLS certificate of the update address plus the checksum in the manifest
  fetched from the same address. Whoever controls this repository's GitHub Pages controls every module. Mitigation:
  two-factor sign-in on the GitHub account; publishing needs a deliberate manual run of the workflow.
- **Adoption window.** For 30 minutes after a start, anyone on the home network who speaks ESPHome's API can adopt
  an unadopted module. That is ESPHome's provisioning model; the window is the protection.
- The test build is open on purpose (see README). It must not be installed for a customer.

## Updates

- `update: platform: http_request` checks `https://jdbressler.github.io/dryer-link/firmware/manifest.json` shortly
  after start-up and every 4 hours. "A different version is offered" means the manifest's `version` is not this
  firmware's `version` (so publishing an older version again rolls every module back).
- `dryer_link` looks every 5 minutes: if a version is offered, the module is on Wi-Fi, nobody is in setup, nothing
  is being learned or checked, and the dryer is not running, paused or in a delayed start, it installs. The module
  restarts into the new version.
- The download is checked against the manifest's md5 before it is used. A new version that fails to start is rolled
  back by the bootloader (ESPHome marks a version good after one minute of running).
- Brake: before each try the module records, in flash, the offered firmware's checksum and a count. While the same
  firmware stays on offer, the next try waits 5 minutes, then 20, 80, 320, up to 24 hours. A different firmware on
  offer starts again at once. If the count cannot be saved, nothing is installed. This keeps a release that never
  takes from rewriting a megabyte of flash every few minutes on every module.
- Hosting: GitHub Pages, deployed by `.github/workflows/build.yml` when it is started by hand with the version typed
  in. GitHub's release downloads were not used because they redirect to long signed addresses, which ESPHome's HTTP
  client needs larger buffers for; ESPHome's documentation recommends Pages for this.
- The firmware trusts the full list of certificate authorities, not ESPHome's shorter default list, so that a change
  of authority at GitHub cannot cut modules off. Checked 2026-11-22: the update address presents a Let's Encrypt
  certificate that chains to ISRG Root X1, which is on the list. The list is as old as the firmware, so publish a
  build on a current ESPHome at least once a year even if nothing else has changed.
- Known limit: a version that starts, runs for a minute, and then cannot reach Wi-Fi or the update address is not
  rolled back and cannot be replaced over the network. Try every release on a module before publishing it.
- Known limit: the update address is fixed in every module. See README, "Publishing a release".

## What is known about GE dryers (2026-11-22)

Measured on the owner's dryer:
- **GTD65GBSJ3WS** (top-load-matching gas dryer): classic bus, address 0x2B. Answers 0x0001 0x0002 0x0008 0x2000
  0x2001 0x2002 0x2007 0x200A 0x200E 0x2010 0x2012 0x201B 0x201C 0x201D 0x2038 0x2039 0x2040 0x2041. Stop and
  Extended Tumble work. Start commands are acknowledged and ignored. The dryer polls 0x6003 at address 0xBF (GE's
  Connect Module) every 5 s. Not checked: the value of its appliance type, and whether it answers a broadcast read
  of it; if it does not, the search falls back to the model number.

From public sources, not measured here:
- **GFD48GSSK0WW** (front-load gas dryer; mguaylam/esphome-gea, `devices/dryer/GFD48GSSK0WW.yaml`): classic bus,
  address 0x24. Answers 0x0001 0x0002 0x2000 0x2001 0x2002 0x2007 0x200A 0x2010 0x2012 0x201B 0x201C and nothing
  else of 2,445 codes scanned; no appliance type; no writable data point found.
- **Older "Hawk" and "Viper" dryers** (GE interface documents as republished in doitaljosh/geabus-documentation):
  classic bus, address 0x24; 0x0001 0x0002 0x2000 0x2001 0x2002 0x2003 (cycle count) 0x2004 (service error codes)
  0x2005 0x2007 0x200A 0x200E 0x200F 0x2010; no appliance type, no door.
- **GE's own adapter** (geappliances/home-assistant-adapter, tiny-gea-api, home-assistant-bridge): GEA3 only; the
  adapter is 0xE4, the appliance 0xC0; it subscribes and falls back to polling. Its compatibility list names GFD,
  GFV, HFD and PFD dryers. A GEA2 dryer does not answer it at all.
- **PFQ97HSPVDS** (Ultrafast Combo; mguaylam/esphome-gea): GEA3; Extended Tumble is read at 0x2053 and written at
  0x2051.

What follows from that, and is covered by pretend dryers in the tests: dryers at 0x24 and 0x2B, with and without an
appliance type, with and without a door on the bus, with and without writable options, on either bus. What does not
follow: that any dryer other than the GTD65GBSJ3WS has been seen working with this firmware.

## Testing
See README, "What is tested". Plan for hardware: a spare module flashed through the adapter and plugged into the
dryer in place of the installed one, which stays untouched as the known-good fallback.

## Not done
- **Hidden Wi-Fi networks** cannot be chosen in the walkthrough.
- **Signed firmware**, and a second update address as a fallback.
- **A trial channel** for releases (publish to a few modules first).
- The final product name.
- Whether the full scan should be startable from the status page.

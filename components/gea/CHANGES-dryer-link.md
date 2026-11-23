# Changes to the vendored `gea` component

Source: https://github.com/mguaylam/esphome-gea at commit `ef16751da77f88fdab11abf1203a96a73187feb4`, MIT licence
(see `LICENSE` in this folder, copyright (c) 2024 mguaylam). Copied here on 2026-11-22 so that every Dryer Link build
uses exactly this revision plus the additions below. Not copied: the upstream repository's `erd_table.h`,
`gea2_erds_fork.txt`, docs, device examples and tests.

Every addition is marked `DL:` in the source. Behaviour is unchanged for a configuration that does not use them.

## gea.h / gea.cpp
1. **Start on demand.** `set_autostart(false)` leaves the hub stopped after `setup()`. `restart(protocol, uart,
   addr_probe_erd)` starts or restarts it on a given UART in GEA2 or GEA3, dropping everything learned so far.
   The old body of `setup()` moved to `start_()`.
2. **Selectable address-probe data point.** GEA2 address discovery broadcast a read of 0x0001 (model number). The data
   point is now a parameter of `restart()`, so the caller can ask for 0x0008 (appliance type) first: GE's definition
   allows only the board that hosts the public data to answer for it. `is_addr_discovery_halted()` and
   `get_addr_candidates()` expose the "more than one board answered" outcome.
3. **Probes.** `probe_erd(erd, retries)` is a read with its own retry budget whose outcome (answered, no answer, or,
   in GEA3, rejected) is reported through `set_probe_callback()`. Answered probes are cached and routed to entities
   like any other read. `is_idle()` tells the caller when the next one can go out.
4. **A lost single attempt is repeated once.** A request with no retries that is hit by a collision, or whose echo
   never returns, gets one more try. Without this a scan would record a collision as "the appliance does not have this
   data point".
5. **Polling control.** `set_polling_enabled(bool)` pauses all GEA2 polling; `set_erd_polled(erd, bool)` removes one
   data point from the schedule or puts it back.
6. **Probe writes.** `probe_write(erd, data, retries)` reports through the same callback whether the appliance
   acknowledged a write, and does not re-read the value afterwards.
7. **GE's fixed address on the new bus.** `restart()` takes the address the first GEA3 subscription is sent to. Stock
   code always used the broadcast address; GE's own adapter subscribes at 0xC0.
8. **Direct addressing.** `set_address(addr)` adopts a GEA2 appliance address without probing, so known addresses can
   be tried one by one on a bus whose boards ignore broadcasts.
9. **"Busy" is not "absent".** A GEA3 read or write answered with GE's "busy" result is reported as `BUSY`, not as a
   refusal.
10. **Polling when a subscription is refused.** A GEA3 appliance that answers the subscription with "no available
    subscriptions" is polled like a GEA2 one (entity data points only), and asked again every 30 s.
11. **Small accessors.** `get_protocol()`, `is_running()`, `get_cached_erds()`, `get_last_rx_ms()`.
   `is_address_resolved()` is now also false while GEA3 is still waiting for its first packet.

## __init__.py
GE's ERD table is loaded only when `erd_lookup` or `gea2_discovery` is on. The stock code loaded it on every build
and ran `git submodule update` in the repository root when the file was missing. Neither option is used here, and the
table is not part of this repository.

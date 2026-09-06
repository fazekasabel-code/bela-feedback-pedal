# Rig I/O bring-up (Phase M0)

Amp OFF for every command that opens an output. Run from `host/`:

```bash
python3 -m rig.io_check <subcommand>
```

## Subcommands

1. `devices` — find the Scarlett; list index, channels, default rate.
2. `xruns` — duplex silence at 48 kHz / 128; fail if any overflow/underflow.
3. `map --out N --i-confirm-amp-is-off` — tone on output N; watch input RMS (channel map).
4. `meter` — input peak/RMS meter; aim for −18…−9 dBFS peak on the guitar.
5. `latency --out N --in M --i-confirm-amp-is-off` — cable loopback round-trip delay.

All accept `--json` **after** the subcommand, for a single machine-readable result object.

`latency` refuses to report a number it did not actually measure: each of the 5 trials must
match the emitted burst above a correlation floor, and fewer than 3 clean trials is an error,
not a result. A wrong channel index or an unpatched cable fails loudly instead of returning
a plausible-looking delay.

## M0 order

`devices` → `xruns` → `map` (each output of interest) → `meter` (set INST gain) →
`latency` (patch out→in with a cable). Write the confirmed map and latency into
`rig-profile.json` only after observation — do not trust labels alone.

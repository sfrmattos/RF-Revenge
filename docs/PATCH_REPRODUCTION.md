# Local patch reproduction

Read the [README](../README.md) for authorization and limitations. This procedure applies only to the exact local framework artifact checked by the patch script.

## Normal path

Run the installer from the repository root:

```sh
./tools/setup.sh
```

It prepares the `rfclown_alt` environment, validates and applies the guarded local patch when needed, rebuilds, and runs the host check. A successful result does not demonstrate RF behavior or compatibility with a different dependency version.

## Advanced manual path

For inspection or troubleshooting, first build once, then run the same guarded tool against the local package:

```sh
pio run -e rfclown_alt
python3 tools/patch_libnet80211.py --library "$HOME/.platformio/packages/framework-arduinoespressif32-libs/esp32/lib/libnet80211.a"
```

The script accepts only the expected archive member, function entry, byte layout, relocation, and disassembly. It creates a `.bak-orig` copy only before its first replacement. If the archive is already patched, it validates that state and exits without writing. Any mismatch is a stop condition: do not force offsets or infer compatibility.

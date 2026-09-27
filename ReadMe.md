# Link Studio Firmware

Official firmware distribution for Link Studio. Download builds from
[Releases](https://github.com/H644b/LinkStudioFirmware/releases).
The browser, Mac and iPhone apps check the latest published release when installing
an update. They validate the board, partition layout, sizes and SHA-256 hashes;
they do not install firmware automatically while trading.

## Supported hardware

The official **Flipper Wi-Fi Developer Board V1**, ESP32-S2-WROVER, 4 MB flash.
The USB/MIDI release supports desktop/browser CDC and direct iPhone USB MIDI.
It is not Flipper Zero system firmware and does not target ESP32-C3/S3 boards.
The independent Flipper GPIO trading engine is still under development; no stable
release claims standalone trading support yet.

## Status LED

The onboard RGB LED is active low: red GPIO6, green GPIO5, blue GPIO4, per the
[official schematic, sheet 2](https://cdn.flipperzero.one/Flipper_Zero_WI-FI_Module_V1_Schematic.PDF).

| Appearance | Current action |
| --- | --- |
| Dim white | Starting |
| Slowly breathing blue | Ready / idle |
| Blue blink | Searching or hosting |
| Steady cyan | Connected |
| Purple double blink | Pokémon offered |
| Fast amber blink | Trade save synchronization; keep connected |
| Green triple blink | Trade replies acknowledged |
| Blinking white | Reading a recovery backup |
| Breathing magenta | Writing/verifying an application firmware update |
| Red triple blink | Error |
| Steady white | Restart requested |

The host supplies trade-phase hints once per second on firmware command `0x17`.
Hints expire after three seconds, reverting to the board's actual radio state.
Maintenance and errors take priority. ROM bootloader flashing cannot use these
patterns because Link Studio firmware is not running then. A completed-trade LED
does not independently prove persistence after restarting the game.

## Build and release

Every push to `main` runs `.github/workflows/release.yml`. The workflow tests the
LED patterns, installs pinned ESP-IDF commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, compiles the ESP32-S2 firmware and uploads
all assets to a draft release. Only after every upload succeeds does it publish
the release as latest. The workflow can also be run manually in Actions.

For a local build, install that ESP-IDF revision and ESP32-S2 tools, source
`export.sh`, then run:

```sh
idf.py -C firmware/flipper-radio -B "$PWD/.build/radio" build
python ci/package.py --build .build/radio --output .build/release \
  --version local-test --source-commit "$(git rev-parse HEAD)"
```

`firmware.bundle.json` contains the manifest and base64-encoded images in one
asset, so an app cannot mix files from different releases. Individual `.bin`
images, `firmware.json` and `SHA256SUMS.txt` are provided for recovery/tools.

First installation uses the ESP32-S2 ROM bootloader and these addresses:
`bootloader.bin` at `0x1000`, `partition-table.bin` at `0x8000`,
`ota_data_initial.bin` at `0xd000`, `pokeldn_radio.bin` at `0x10000`.
Later application-mode updates write only the inactive OTA slot, verify it,
and restart. To enter ROM recovery manually, hold BOOT, tap RESET, release BOOT.
Use a desktop USB connection for the first installation and ROM recovery.

Source is synchronized from the Link Studio workspace by
`scripts/publish-firmware.py` after successful local builds. The synchronizer
uses a source-only allowlist; it never publishes keys, saves, backups or captures.
See `LICENSE` and `THIRD_PARTY.md` for upstream notices.

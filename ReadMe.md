# Link Studio Firmware

Official firmware distribution for Link Studio. Download builds from
[Releases](https://github.com/H644b/LinkStudioFirmware/releases).
The browser, Mac and iPhone apps check the latest published release when installing
an update. They validate the board, partition layout, sizes and SHA-256 hashes;
they do not install firmware automatically while trading.

## Supported hardware

The official **Flipper Wi-Fi Developer Board V1**, ESP32-S2-WROVER, 4 MB flash.
The release supports desktop/browser CDC, direct iPhone USB MIDI, and GPIO UART
for the Flipper app.
It is not Flipper Zero system firmware and does not target ESP32-C3/S3 boards.
The integrated GPIO engine uses the Flipper for controls and runs networking on the
board. A build advertises `standalone-v1` only after its worker starts successfully.
Hardware verification is recorded separately from the CI image manifest.
On 2026-09-27, a local build of this runtime completed two consecutive real Switch
trades through GPIO, including cancel/reoffer and normal Stop. The user confirmed
both Pokémon persisted after reopening Z-A. Stop during saving and controller loss
have offline coverage; they were not physically verified in that session.
See the [Flipper setup and verification notes](https://github.com/H644b/Pokemon-Spoofer/tree/main/flipper).

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

Desktop and iPhone hosts supply trade-phase hints once per second on firmware command `0x17`.
Hints expire after three seconds, reverting to the board's actual radio state.
The standalone GPIO worker drives those states directly.
Maintenance and errors take priority. ROM bootloader flashing cannot use these
patterns because Link Studio firmware is not running then. A completed-trade LED
does not independently prove persistence after restarting the game.

## Updating from a Flipper Zero

Install this firmware once over desktop USB, then open **Wi-Fi and updates →
Choose nearby network** in the Link Studio Flipper app. The board scans 2.4 GHz
networks. Select yours and enter its WPA2 password (8–63 ASCII characters); open
networks skip password entry. WPA3-only and captive-portal networks are not supported.
Use the keyboard's Save key to start the connection and update check.

After successful connection and DHCP, up to eight networks are saved in the board's
NVS flash. **Saved networks** lets you reconnect, replace a password or forget a
network. Passwords are not returned in network lists, written to Flipper SD or included
in releases. They are stored persistently on the board without flash encryption;
temporary copies are cleared after use. The Flipper keyboard displays entered characters.
The legacy command remains temporary and does not save credentials.

The board obtains an IP address and synchronizes time before HTTPS certificate
validation. It reads this repository's latest release metadata, verifies the manifest
and application SHA-256 hashes, and writes only the inactive OTA partition. Progress
appears on the Flipper. Keep the board powered until it restarts. The current running
slot remains usable after a failed download. Boot rollback stays enabled until the
new build reaches USB and Wi-Fi startup. Updates are refused during a trade or another
firmware write. Wi-Fi is disconnected and the normal radio mode restored on failure.

The Flipper updater requires `uart-v1` and `wifi-update-v1`; scanning and saved networks
also require `wifi-profiles-v1`. Older boards show a one-time manual network entry
option to download an upgrade. GPIO trading additionally requires `standalone-v1`
and a prepared Pokémon recipe on the Flipper's SD card.

## Build and release

Every push to `main` runs `.github/workflows/release.yml`. The workflow tests the
LED patterns, saved profile handling, network message parsing, action guards and the
release-validation contract, installs pinned ESP-IDF commit
`fff9895c82d744c7237be8847347bdd1b07c6643` and checksum-verified Zstandard 1.5.7,
compiles the ESP32-S2 firmware and uploads
all assets to a draft release. Only after every upload succeeds does it publish
the release as latest. The workflow can also be run manually in Actions.

For a local build, install that ESP-IDF revision and ESP32-S2 tools, source
`export.sh`, then prepare the same bounded Z-A connection-profile asset used by
the Link Studio web/iPhone build. This is not a console root-key file. CI receives
its gzip/base64 form through the repository Actions secret
`LS_HOST_PROFILES_GZIP_B64`; missing or malformed data fails the build rather than
silently releasing firmware without standalone support. Profile contents are not
logged or copied into source control. For a local JSON asset at
`.build/browser-host-profile.json`, run:

```sh
python ci/prepare-zstd.py
python ci/prepare-host-profile.py --input .build/browser-host-profile.json \
  --output .build/profiles/ls_host_profiles.h
idf.py -C firmware/flipper-radio -B "$PWD/.build/radio" \
  -D LINK_STUDIO_GPIO=ON -D LINK_STUDIO_ZSTD="$PWD/.build/zstd-1.5.7" \
  -D LINK_STUDIO_PROFILES_DIR="$PWD/.build/profiles" build
python ci/package.py --build .build/radio --output .build/release \
  --version local-test --source-commit "$(git rev-parse HEAD)" --standalone
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

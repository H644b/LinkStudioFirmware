# Third-party code and data

| Component | Source / revision | Distribution in this project |
| --- | --- | --- |
| pokeldn | [Decryptu/pokeldn](https://github.com/Decryptu/pokeldn), `478a76d6d8f8c382f7071f26e73ab2eedbcef4b6` | AGPL-3.0; fetched by bootstrap, patched by `patches/pokeldn.patch`; original license retained in checkout |
| LDN | pokeldn's `vendor/LDN` at that same commit | GPL-3.0; original upstream is [kinnay/LDN](https://github.com/kinnay/LDN); notices retained in fetched source |
| PKHeX | [kwsch/PKHeX](https://github.com/kwsch/PKHeX), `bbef47e0bd02e2b54411ac7de4b886342dee4c56` | GPL-3.0; selected source/data and license in `research/pkhex-reference/`; generated Gen9a form catalog uses these references |
| ESP-IDF | [espressif/esp-idf](https://github.com/espressif/esp-idf), `fff9895c82d744c7237be8847347bdd1b07c6643` | SDK fetched locally; its component-specific notices apply |
| ESP USB CDC component | Retained port in `firmware/flipper-radio/components/esp_usb_cdc_rom_console/` | Original license and local-patch note included; currently inactive |
| TinyUSB components | ESP-IDF component manager, locked by `dependencies.lock` | Fetched at build time; upstream component licenses retained locally |
| cJSON | [espressif/cjson 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19) | MIT; pinned by the component manager; firmware releases include `cJSON-LICENSE.txt` |
| Zstandard | [facebook/zstd v1.5.7](https://github.com/facebook/zstd/releases/tag/v1.5.7) | BSD-3-Clause; pinned and checksum-verified by `scripts/prepare-flipper-zstd.py`; decompression sources used by the standalone GPIO firmware; retain the fetched `LICENSE` in binary distributions |
| Ryubing / LdnServer | Revisions and upstream URLs in `patches/sources.json` | Optional research patches only; source/binaries not bundled |
| React / React DOM | Versions locked in `web/package-lock.json` | MIT; browser UI dependencies fetched by npm |
| Lucide React | Version locked in `web/package-lock.json` | ISC; interface icons fetched by npm |
| Pyodide | Version locked in `web/package-lock.json` | MPL-2.0; browser Python runtime, with separately licensed bundled Python packages |
| esptool-js | Version locked in `web/package-lock.json` | Apache-2.0; browser ROM backup, flashing and on-board checksum verification |

The ESP32-S2 radio source is adapted from pokeldn's ESP32 firmware. Its local changes are
documented in the firmware README. Python package versions are in `requirements.txt`; their
own upstream licenses continue to apply. Showdown text import is a local parser informed by
the public [Showdown format implementation](https://github.com/smogon/pokemon-showdown/blob/master/sim/teams.ts);
no Showdown service connection is made when importing.

The repository does not distribute Nintendo games, console keys, saves, decrypted game
executables, captured Pokémon records, or original board flash backups. Pokémon names and
related marks belong to their respective owners. All original third-party notices must be
retained in distributions of the corresponding code/data.

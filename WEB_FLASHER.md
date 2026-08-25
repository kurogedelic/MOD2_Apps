# MOD2 Web Flasher

The `web/` directory is a static GitHub Pages app that flashes the prebuilt MOD2 firmware directly to the Seeed XIAO RP2350 over WebUSB/PICOBOOT.

## Enable it

The repository includes the complete Pages workflow as `.github/pages-workflow.yml.example`.

1. Copy that file to `.github/workflows/pages.yml` and commit it.
2. In **Settings → Pages**, set **Source** to **GitHub Actions** if it is not already selected.
3. Push to `main`. The workflow compiles every `MOD2_*` sketch, publishes its UF2 under `firmware/`, and deploys the web app.

The resulting project site is expected at:

`https://kurogedelic.github.io/MOD2_Apps/`

## Flashing

Use a Chromium browser with WebUSB support.

1. Disconnect the Eurorack power ribbon before connecting USB.
2. Put the XIAO RP2350 into BOOTSEL mode: hold BOOT, tap RESET, release BOOT.
3. Pick an app and press **Flash to MOD2**.
4. Select the RP2350 boot device in the browser USB chooser.

The web app accepts only the RP2350 PICOBOOT device and validates the decoded UF2 flash range before erasing or writing.

## Firmware builds

The Pages workflow uses Arduino-Pico 6.0.0 with:

`rp2040:rp2040:seeed_xiao_rp2350:freq=150,arch=arm`

Every manifest entry must produce a matching `firmware/MOD2_*.uf2`; deployment fails if the counts differ. SHA-256 checksums are generated alongside the UF2 files.

## BIAS calibration

The web flasher publishes the repository's prebuilt constants. `BIAS` is still per-unit. Run `MOD2_Calibrate` first on a new module and update the relevant sketch if the measured value differs.

## WebUSB implementation

The browser flasher uses the MIT-licensed `pico⚡flash` PICOBOOT implementation, pinned to commit `678355430aff0ee9efa6d552fb81832a91d89ef4` rather than tracking a moving CDN branch.

# Vendored components (ESP-IDF build only)

`Arduino_GFX` (the AMOLED driver), `NimBLE-Arduino` (the BLE stack) and `SdFat`
(the microSD filesystem) are Arduino-only libraries that aren't in the ESP
Component Registry, so the ESP-IDF build vendors them here. Each folder already
has a `CMakeLists.txt` wrapper — you just add the library sources next to it.

**One-time setup** (from the `firmware/` directory):

```bash
git clone --depth 1 https://github.com/moononournation/Arduino_GFX.git   /tmp/agfx
git clone --depth 1 https://github.com/h2zero/NimBLE-Arduino.git          /tmp/nimble
git clone --depth 1 https://github.com/greiman/SdFat.git                  /tmp/sdfat

# copy the library contents in so that components/<lib>/src/ exists
cp -r /tmp/agfx/src      components/Arduino_GFX/
cp -r /tmp/nimble/src    components/NimBLE-Arduino/
cp -r /tmp/sdfat/src     components/SdFat/
```

After that, `components/<lib>/src/*` exists for all three and `idf.py build`
picks them up via the wrapper `CMakeLists.txt`.

> These directories are intentionally empty except for the wrappers (the
> library sources are git-ignored). The **PlatformIO build does not use this
> folder at all** — it resolves the libraries automatically from `lib_deps`.
>
> Check out the SAME versions the PlatformIO build pins (see `../platformio.ini`):
> Arduino_GFX 1.6.7, NimBLE-Arduino 2.5.0, SdFat 2.3.1 — those are the versions
> the hardware-verified image was bench-tested against, so clone the matching
> tag rather than master. If you later `git submodule add` them instead of
> copying, point the submodules at those tags.

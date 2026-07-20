# Vendored components (ESP-IDF build only)

`Arduino_GFX` (the AMOLED driver) and `NimBLE-Arduino` (the BLE stack) are
Arduino-only libraries that aren't in the ESP Component Registry, so the
ESP-IDF build vendors them here. Each folder already has a `CMakeLists.txt`
wrapper — you just add the library sources next to it.

**One-time setup** (from the `firmware/` directory):

```bash
git clone --depth 1 https://github.com/moononournation/Arduino_GFX.git   /tmp/agfx
git clone --depth 1 https://github.com/h2zero/NimBLE-Arduino.git          /tmp/nimble

# copy the library contents in so that components/<lib>/src/ exists
cp -r /tmp/agfx/src      components/Arduino_GFX/
cp -r /tmp/nimble/src    components/NimBLE-Arduino/
```

After that, `components/Arduino_GFX/src/*` and `components/NimBLE-Arduino/src/*`
exist and `idf.py build` picks them up via the wrapper `CMakeLists.txt`.

> These two directories are intentionally empty except for the wrapper (the
> library sources are git-ignored). The **PlatformIO build does not use this
> folder at all** — it resolves both libraries automatically from `lib_deps`.
>
> Pin the same versions the PlatformIO build uses (see `../platformio.ini`):
> Arduino_GFX ^1.4.9, NimBLE-Arduino ^2.1.0. If you later `git submodule add`
> them instead of copying, point the submodule at those tags.

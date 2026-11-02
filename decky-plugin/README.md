# SteamFrame Passthrough Denoise (Decky Loader plugin)

A [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) plugin exposing the
passthrough denoise layer's `config.ini` settings in the Quick Access Menu: a filter-enabled
toggle (`enabled` -- raw passthrough, no GPU dispatch, when off) plus the four shader tunables
(`temporal_alpha`, `motion_threshold`, `sharpness_weight`, `noise_floor`, documented in the
repo-root `config.ini` and `SETUP.md`) as sliders.

There is no IPC with the running layer. `src/layer.cpp` already polls `config.ini`'s mtime and
hot-reloads it within a frame or two (see `SETUP.md`), so this plugin's entire job is reading and
writing that one file at `~/.config/steamframe-passthrough/config.ini` -- `main.py` has no other
responsibility, and `src/layer.cpp` needed no socket/IPC to support it (an earlier draft PR,
superseded by this plugin, added a UNIX-socket control protocol for the same bypass toggle; see
that PR's discussion for why it wasn't worth the added complexity for this use case).

## Layout

- `main.py` -- Python backend (`Plugin.get_config`/`Plugin.set_param`, called from the frontend via
  `@decky/api`'s `callable()`). Parses/writes the ini file with the same relaxed rules as
  `sf::Config::load` in `include/denoise.hpp` (skip blank/`;`/`#` lines, split on first `=`),
  overwriting an existing key's line in place and preserving every other line (comments, section
  header) untouched.
- `src/index.tsx` -- frontend: a `ToggleField` for `enabled` plus four `SliderField`s (0..1 range)
  from `@decky/ui`, all synced to the backend on every change.
- `plugin.json`, `package.json`, `rollup.config.js`, `tsconfig.json` -- scaffolding from the
  official [decky-plugin-template](https://github.com/SteamDeckHomebrew/decky-plugin-template),
  trimmed down (no router/event-listener example code, no `_root` flag since this plugin only
  touches a file under the user's own `$HOME`).

## Building

```bash
pnpm install
pnpm run build       # -> dist/index.js
```

Verified in this repo: `pnpm install && pnpm run build` succeeds against the real `@decky/ui`
4.16.4 / `@decky/api` 1.1.3 packages, and `main.py`'s config read/write logic was exercised against
a stubbed `decky` module (defaults when missing, write+read round trip, overwrite without
duplicating lines, out-of-range clamping, unknown-key rejection, and parsing/preserving a
real-style commented `config.ini`) -- all passing.

**Not verified:** this plugin has never been loaded into an actual Decky Loader instance (no
SteamOS/Steam Deck/Steam Frame hardware or Decky runtime available in this dev environment), so
the frontend has not actually been clicked through in the real Quick Access Menu.

## Deploying

Standard Decky plugin deployment: copy `dist/`, `main.py`, `plugin.json`, and `package.json` into
`homebrew/plugins/SteamFrame-Passthrough-Denoise/` on the device (or use the Decky CLI / VSCode
tasks from the plugin template for a dev-mode deploy with hot reload). See the
[Decky plugin dev docs](https://wiki.deckbrew.xyz/) for the current recommended workflow.

## Caveat: Quick Access Menu support on Steam Frame / in VR is itself unverified

Decky Loader's own Steam Frame (Valve's VR headset) support was, as of
[decky-loader#1004](https://github.com/SteamDeckHomebrew/decky-loader/pull/1004), explicitly "VERY
VERY INCOMPLETE (no VR APIs yet)". Whether the Quick Access Menu overlay -- and therefore this
plugin's GUI -- actually renders while wearing the headset in a VR session depends on that support
maturing; this plugin was written to be ready for it, not to prove it already works. If Decky's
QAM only shows up in SteamOS desktop/flat-screen mode on this hardware, this plugin still works
there (editing the same `config.ini` the layer reads), it just wouldn't be "usable while wearing
the HMD" as originally asked for.

## If `config.ini`'s fields ever change

Keep three places in sync: `include/denoise.hpp`'s `sf::Config`, the repo-root `config.ini`'s
comments/defaults, and `PARAMS` in `main.py` / the `Params` interface + `DEFAULTS` in
`src/index.tsx`.

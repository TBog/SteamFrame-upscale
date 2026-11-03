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

A build once failed with a cryptic `RangeError: Invalid string length` loading `react-icons`'
large icon bundle, on Ubuntu 24.04 with its default `apt install nodejs` (18.19) active. It could
not be reliably reproduced under controlled retesting afterward, including with that exact Node
version deliberately put first on `PATH` again -- most likely a one-off filesystem hiccup reading
that large file over the WSL/NTFS bridge the first time, not an actual Node-version dependency.
There's no enforced version guard here: `package.json`'s `engines` field and a `preinstall`
version check were both tried and found to not actually fire (`pnpm`, at least the standalone
binary distribution installed via `npm install -g pnpm`, appears to always run lifecycle/build
scripts under its own consistently-resolved Node rather than whatever is first on the invoking
shell's `PATH`, so there may be nothing for a guard to meaningfully check here in the first place).
CI (`.github/workflows/ci.yml`) provisions Node 20 via `actions/setup-node` regardless, as a
reasonable default independent of this unresolved question.

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

## Workaround: reaching Decky's UI on Frame before official VR support lands

Via [decky-loader#996](https://github.com/SteamDeckHomebrew/decky-loader/issues/996), confirmed
straight from `injector.py` in the decky-loader source (not just the issue comment) -- so this is
how Decky's own code already talks to Steam, not a guess:

- Steam's own UI runs in a CEF (Chromium) process that exposes the standard [Chrome DevTools
  Protocol](https://chromedevtools.github.io/devtools-protocol/) on `http://localhost:8080`.
  `GET /json` lists the open CDP targets; decky-loader looks for the one it calls "GamepadUI" --
  title in `SharedJSContext` / `Steam Shared Context presented by Valve™` / `Steam` / `SP`, and URL
  containing `steamloopback.host/routes/` or `steamloopback.host/index.html` -- and opens a
  WebSocket to that target's `webSocketDebuggerUrl` to send `Runtime.evaluate` commands. This is
  exactly the shared JS context decky injects its own frontend (and therefore every plugin's panel,
  including this one) into.
- `window.DFL` is the global that injected code (decky's own, and by extension this is the same
  object plugins reach via `@decky/ui`'s `Navigation` import) uses for SteamUI's router. Per a
  decky-loader maintainer's reply in that same issue thread (no stable comment anchor to link --
  see the issue itself): `DFL.Navigation.Navigate("/decky/settings")` force-navigates straight to
  Decky's settings page, bypassing the fact that Frame has no QAM button/dashboard icon wired up
  for it yet (the actual gap [decky-loader#1004](https://github.com/SteamDeckHomebrew/decky-loader/pull/1004)
  is meant to close).

To actually run that expression against a Frame headset, the practical recipe (same one the
Steam Deck homebrew community already uses for desktop/flat-screen devtools access, not something
specific to this plugin):

1. Get a shell on the headset (SteamOS Developer Mode -> SSH, same as Steam Deck -- **not
   independently confirmed that Frame exposes this identically to Deck**, since Frame hardware is
   very new; treat this step as the main unverified assumption here).
2. Tunnel the CDP port to your desktop: `ssh -L 8080:localhost:8080 <frame-ssh-target>`. This is
   plain SSH port-forwarding, independent of Decky's own `cef_forward` setting (which is a
   decky-settings-page toggle for the same thing over the network -- not useful here since reaching
   that settings page is the whole problem being worked around).
3. On your desktop, open Chrome/Chromium's `chrome://inspect/#devices`, add `localhost:8080` under
   "Discover network targets", and open DevTools on the GamepadUI/SharedJSContext target found
   above.
4. In that DevTools window's Console tab, run `DFL.Navigation.Navigate("/decky/settings")`.

**What this does and doesn't confirm:** this reaches Decky's own settings page, not necessarily a
direct route to *this* plugin's panel specifically -- no such per-plugin URL is documented or
confirmed. Once inside Decky's shell this way, the normal plugin list/sidebar should still work for
navigating to this plugin's panel from there, same as on desktop, but that hasn't been tried
either. Treat this whole section as a developer debugging path for confirming the GUI *can* render
in a VR session at all, not a real end-user way to launch it.

## If `config.ini`'s fields ever change

Keep three places in sync: `include/denoise.hpp`'s `sf::Config`, the repo-root `config.ini`'s
comments/defaults, and `PARAMS` in `main.py` / the `Params` interface + `DEFAULTS` in
`src/index.tsx`.

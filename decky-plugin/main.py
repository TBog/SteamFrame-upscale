# Decky Loader backend: reads/writes the passthrough denoise layer's config.ini. There is no IPC
# with the running layer -- src/layer.cpp already polls this exact file's mtime and hot-reloads it
# within a frame or two (see SETUP.md), so writing the file is the entire integration.
import os

import decky

# Matches get.sh / CMakeLists.txt's install step, and src/layer.cpp's own fallback path when
# SF_CONFIG is unset and no config.ini exists in the launching app's cwd (the common case when
# launched from Steam). If this ever drifts from those, the slider changes silently stop reaching
# the layer -- see the "Fix config.ini default path mismatch" commit for exactly that bug.
CONFIG_PATH = os.path.join(decky.DECKY_USER_HOME, ".config", "steamframe-passthrough", "config.ini")

# (min, max, default), mirroring sf::Config in include/denoise.hpp and config.ini's comments.
PARAMS = {
    "temporal_alpha": (0.0, 1.0, 0.15),
    "motion_threshold": (0.0, 1.0, 0.08),
    "sharpness_weight": (0.0, 1.0, 0.8),
    "noise_floor": (0.0, 1.0, 0.02),
}


def _parse_ini_lines(lines):
    """Same relaxed parsing as sf::Config::load: skip blanks/comments, split on first '='."""
    values = {}
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped[0] in ";#" or "=" not in stripped:
            continue
        key, _, raw = stripped.partition("=")
        values[key.strip()] = raw.strip()
    return values


def _read_config() -> dict:
    result = {key: default for key, (_, _, default) in PARAMS.items()}
    try:
        with open(CONFIG_PATH, "r") as f:
            lines = f.readlines()
    except OSError:
        return result
    for key, raw in _parse_ini_lines(lines).items():
        if key in PARAMS:
            try:
                result[key] = float(raw)
            except ValueError:
                pass
    return result


def _write_param(key: str, value: float) -> float:
    lo, hi, _ = PARAMS[key]
    value = max(lo, min(hi, value))
    try:
        with open(CONFIG_PATH, "r") as f:
            lines = f.readlines()
    except OSError:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        lines = ["[passthrough]\n"]

    new_line = f"{key} = {value}\n"
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped and stripped[0] not in ";#" and "=" in stripped and stripped.partition("=")[0].strip() == key:
            lines[i] = new_line
            break
    else:
        lines.append(new_line)

    with open(CONFIG_PATH, "w") as f:
        f.writelines(lines)
    return value


class Plugin:
    async def get_config(self) -> dict:
        return _read_config()

    async def set_param(self, key: str, value: float) -> float:
        if key not in PARAMS:
            raise ValueError(f"unknown passthrough denoise parameter: {key}")
        return _write_param(key, float(value))

    async def _main(self):
        decky.logger.info("SteamFrame passthrough denoise plugin loaded (config: %s)" % CONFIG_PATH)

    async def _unload(self):
        decky.logger.info("SteamFrame passthrough denoise plugin unloaded")

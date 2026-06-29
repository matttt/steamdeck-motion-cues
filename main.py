from __future__ import annotations

import asyncio
import grp
import json
import os
import pwd
import subprocess
from typing import Any, Callable, Optional

import decky

PLUGIN_NAME = "MotionCueDots"
BINARY_NAME = "SteamDeckMotionCues"
DECK_USER = "deck"
INPUT_GROUP = "input"
CONFIG_PATH = f"/home/deck/.config/{PLUGIN_NAME}/config.json"

DEFAULT_SETTINGS = {
    "enabled": False,
    "dotColor": "#ffffff",
    "dotAmount": 4,
    "dotSize": 1.00,
    "transparency": 0.30,
    "response": 1.5,
    "waveMotion": False,
    "simulateMotion": False,
}

def ensure_config_dir():
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)


def build_backend_env() -> dict[str, str]:
    env = os.environ.copy()
    env["DISPLAY"] = ":0"
    env["XDG_RUNTIME_DIR"] = "/run/user/1000"
    env["HOME"] = "/home/deck"
    env["USER"] = DECK_USER
    env["LOGNAME"] = DECK_USER
    env.pop("XAUTHORITY", None)
    return env


def deck_identity() -> tuple[int, int, list[int]]:
    deck = pwd.getpwnam(DECK_USER)
    input_gid = grp.getgrnam(INPUT_GROUP).gr_gid
    groups = os.getgrouplist(DECK_USER, deck.pw_gid)
    if input_gid not in groups:
        groups.append(input_gid)
    return deck.pw_uid, deck.pw_gid, groups


def ensure_backend_owner(binary_path: str, deck_uid: int, deck_gid: int):
    # we want to make the user 'deck' the owner of the binary 
    # the binary needs to run as 'deck' to access the gamescope instance
    stat = os.stat(binary_path)
    if stat.st_uid == deck_uid and stat.st_gid == deck_gid:
        return
    os.chown(binary_path, deck_uid, deck_gid)
    decky.logger.info("Set renderer owner to %s:%s: %s", DECK_USER, DECK_USER, binary_path)


def drop_to_deck_preexec(deck_uid: int, deck_gid: int, groups: list[int]) -> Callable[[], None]:
    def drop_to_deck():
        # Privilege-dropping order matters: set supplementary groups before uid.
        os.setgroups(groups)
        os.setgid(deck_gid)
        os.setuid(deck_uid)

    return drop_to_deck


def clamp_number(value: Any, default: float, minimum: float, maximum: float) -> float:
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return default
    return min(max(numeric, minimum), maximum)


def normalize_color(value: Any) -> str:
    if not isinstance(value, str):
        return DEFAULT_SETTINGS["dotColor"]

    color = value.strip().lower()
    if not color.startswith("#"):
        color = f"#{color}"
    if len(color) != 7:
        return DEFAULT_SETTINGS["dotColor"]
    try:
        int(color[1:], 16)
    except ValueError:
        return DEFAULT_SETTINGS["dotColor"]
    return color


def sanitize_settings(settings: dict[str, Any]) -> dict[str, Any]:
    sanitized = DEFAULT_SETTINGS.copy()
    sanitized.update(settings)
    sanitized["enabled"] = bool(sanitized["enabled"])
    sanitized["waveMotion"] = bool(sanitized["waveMotion"])
    sanitized["simulateMotion"] = bool(sanitized["simulateMotion"])
    sanitized["dotColor"] = normalize_color(sanitized["dotColor"])
    sanitized["dotAmount"] = int(clamp_number(sanitized["dotAmount"], DEFAULT_SETTINGS["dotAmount"], 2, 24))
    sanitized["dotSize"] = round(clamp_number(sanitized["dotSize"], DEFAULT_SETTINGS["dotSize"], 0.25, 3.0), 3)
    sanitized["transparency"] = round(clamp_number(sanitized["transparency"], DEFAULT_SETTINGS["transparency"], 0.05, 1.0), 3)
    sanitized["response"] = round(clamp_number(sanitized["response"], DEFAULT_SETTINGS["response"], 0.25, 3.0), 3)
    return sanitized


def color_to_rgb(color: str) -> tuple[float, float, float]:
    color_int = int(color[1:], 16)
    return (
        ((color_int >> 16) & 0xFF) / 255.0,
        ((color_int >> 8) & 0xFF) / 255.0,
        (color_int & 0xFF) / 255.0,
    )


class MotionCuesBackend:
    def __init__(self):
        self.proc: Optional[subprocess.Popen[bytes]] = None
        self.config = DEFAULT_SETTINGS.copy()

    def binary_path(self) -> str:
        local_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", BINARY_NAME)
        if os.path.exists(local_path):
            return local_path
        return f"/home/deck/homebrew/plugins/{PLUGIN_NAME}/bin/{BINARY_NAME}"

    def save_config(self):
        ensure_config_dir()
        with open(CONFIG_PATH, "w") as f:
            json.dump(self.config, f, indent=2)

    def load_config(self):
        ensure_config_dir()
        loaded: dict[str, Any] = {}
        if os.path.exists(CONFIG_PATH):
            try:
                with open(CONFIG_PATH) as f:
                    loaded = json.load(f)
            except Exception as e:
                decky.logger.error("Failed to load config", exc_info=e)
        self.config = sanitize_settings(loaded)
        self.save_config()

    def renderer_args(self) -> list[str]:
        red, green, blue = color_to_rgb(self.config["dotColor"])
        return [
            self.binary_path(),
            "--dot-amount",
            str(self.config["dotAmount"]),
            "--dot-size",
            f"{self.config['dotSize']:.3f}",
            "--opacity",
            f"{self.config['transparency']:.3f}",
            "--response",
            f"{self.config['response']:.3f}",
            "--color-r",
            f"{red:.4f}",
            "--color-g",
            f"{green:.4f}",
            "--color-b",
            f"{blue:.4f}",
            "--wave",
            "1" if self.config["waveMotion"] else "0",
            "--simulate-motion",
            "1" if self.config["simulateMotion"] else "0",
        ]

    async def create(self):
        if self.proc is not None or not self.config["enabled"]:
            return

        binary = self.binary_path()
        if not os.path.exists(binary):
            decky.logger.error("Renderer binary does not exist: %s", binary)
            return

        decky.logger.info("Starting renderer with settings: %s", self.config)
        preexec_fn = None
        if os.geteuid() == 0:
            try:
                deck_uid, deck_gid, groups = deck_identity()
                ensure_backend_owner(binary, deck_uid, deck_gid)
                # our plugin runs as root, mainly to add the input group to the deck user before running the binary as 'deck'
                preexec_fn = drop_to_deck_preexec(deck_uid, deck_gid, groups)
            except KeyError as e:
                decky.logger.error("Cannot drop renderer privileges: %s", e)
                return
            except OSError as e:
                decky.logger.error("Cannot set renderer owner: %s", e)
                return

        self.proc = subprocess.Popen(
            self.renderer_args(),
            env=build_backend_env(),
            preexec_fn=preexec_fn,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    async def destroy(self):
        if self.proc is None:
            return

        decky.logger.info("Stopping renderer")
        self.proc.kill()
        output = self.proc.stdout.read() if self.proc.stdout else b""
        if output:
            decky.logger.info("Renderer output: %s", output.decode(errors="replace"))
        self.proc = None

    async def get_settings(self):
        return self.config

    async def save_settings(self, settings):
        was_running = self.proc is not None
        self.config = sanitize_settings({**self.config, **settings})
        self.save_config()

        if was_running:
            await self.destroy()
            if self.config["enabled"]:
                await self.create()

        return self.config

    async def reload_config(self):
        was_running = self.proc is not None
        if was_running:
            await self.destroy()
        self.load_config()
        if was_running and self.config["enabled"]:
            await self.create()
        return self.config

    async def _main(self):
        self.load_config()
        decky.logger.info("Started")

        while True:
            if self.proc is not None and (result := self.proc.poll()) is not None:
                output = self.proc.stdout.read() if self.proc.stdout else b""
                decky.logger.error("Renderer exited with code %s: %s", result, output.decode(errors="replace"))
                self.proc = None
            await asyncio.sleep(0.1)

    async def _unload(self):
        decky.logger.info("Unloading")
        await self.destroy()


# Decky instantiates this class and dispatches frontend `call(route, ...args)` to its
# methods with positional arguments, so the methods must keep their real signatures.
class Plugin(MotionCuesBackend):
    pass

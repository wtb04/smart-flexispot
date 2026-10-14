import json
import os
from dataclasses import asdict, dataclass, fields
from pathlib import Path

import gi

gi.require_version("Secret", "1")
from gi.repository import GLib, Secret  # noqa: E402


@dataclass
class Settings:
    panel: str = "smart-flexispot"
    sharing: bool = True
    claude: bool = True


def default_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config"
    return Path(base) / "desklink" / "settings.json"


def load(path: Path) -> Settings:
    try:
        stored = json.loads(path.read_text())
    except (OSError, ValueError):
        return Settings()
    if not isinstance(stored, dict):
        return Settings()
    result = Settings()
    for field in fields(Settings):
        value = stored.get(field.name)
        if isinstance(value, type(getattr(result, field.name))):
            setattr(result, field.name, value)
    return result


def save(settings: Settings, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(asdict(settings), indent=2) + "\n")
    temporary.replace(path)


class SecretKeyStore:
    _schema = Secret.Schema.new("nl.w-tb.desklink", Secret.SchemaFlags.NONE,
                                {"service": Secret.SchemaAttributeType.STRING,
                                 "account": Secret.SchemaAttributeType.STRING})
    _attributes = {"service": "nl.w-tb.desklink", "account": "panel"}

    def read(self) -> str:
        try:
            return Secret.password_lookup_sync(self._schema, self._attributes, None) or ""
        except GLib.Error:  # no Secret Service running, or its collection locked
            return ""

    def write(self, hex_key: str) -> None:
        if hex_key:
            Secret.password_store_sync(self._schema, self._attributes, Secret.COLLECTION_DEFAULT,
                                       "Desk Link panel key", hex_key, None)
        else:
            Secret.password_clear_sync(self._schema, self._attributes, None)

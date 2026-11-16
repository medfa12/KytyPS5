"""Resolve machine-local inputs without embedding a workstation layout."""
import os
from pathlib import Path


def configured_path(value=None, *, env="KYTY_FIRMWARE_ROOT"):
    """Explicit option wins over environment; missing inputs fail before writes."""
    selected = value if value is not None else os.environ.get(env)
    if selected is None or not str(selected).strip():
        raise SystemExit(f"Supply an explicit input path or set {env}.")
    return Path(os.path.expandvars(str(selected))).expanduser().resolve()

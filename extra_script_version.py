# extra_script_version.py (Project root)
import os
import subprocess
import SCons

def get_git_version():
    """Semantic Git version: v1.0-5-gabcdef-dirty (or fallback)."""
    try:
        # git describe: tags + commits + hash + dirty flag
        desc = subprocess.check_output([
            "git", "describe", "--tags", "--dirty", "--always"
        ]).decode("utf-8").strip()
        return desc  # e.g., "v1.0", "v1.0-3-gabcdef", "gabcdef"
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"

Import("env")
env['PIOENV_GIT_VERSION'] = get_git_version()
print(f"=== OTA Version: {env['PIOENV_GIT_VERSION']} ===")
"""Emit pinned CI dependencies; never download them from a consumer workspace."""
import json
from pathlib import Path

if __name__ == "__main__":
    for name, revision in json.loads(Path(__file__).with_name("dependencies.json").read_text()).items():
        if name not in {"utils", "engine"} or len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
            raise SystemExit("Expected full Git revisions for utils and engine")
        print(f"{name}={revision}")

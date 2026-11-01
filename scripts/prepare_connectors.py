"""Download reviewed source revisions and install without requiring Git."""
import json
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REVISIONS = {
    "xuexitong": ("DanMo661/xuexitong-cli", "74b843ad4a3439a6ee5ba33f6a3288d9be6ed990"),
    "zhs": ("pjm314159/zhs", "fcebefb5f1cb054fa3bd8b516e788f3abfd53bdd"),
}


def download(url, path):
    if path.exists():
        return
    print("Downloading " + url, flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "Keqi-build/0.2"})
    partial = path.with_suffix(path.suffix + ".part")
    with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
    partial.replace(path)


def sources():
    cache = ROOT / ".deps"
    cache.mkdir(exist_ok=True)
    paths = []
    for name, (repo, revision) in REVISIONS.items():
        target = cache / name
        if not target.exists():
            archive = cache / f"{name}-{revision}.zip"
            download(f"https://codeload.github.com/{repo}/zip/{revision}", archive)
            with zipfile.ZipFile(archive) as source:
                prefix = source.namelist()[0].split("/")[0] + "/"
                target.mkdir()
                for entry in source.infolist():
                    relative = entry.filename.removeprefix(prefix)
                    if not relative:
                        continue
                    dest = (target / relative).resolve()
                    if not dest.is_relative_to(target.resolve()):
                        raise RuntimeError("Unsafe archive entry")
                    if entry.is_dir():
                        dest.mkdir(parents=True, exist_ok=True)
                    else:
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        dest.write_bytes(source.read(entry))
        paths.append(target)
    return paths


if __name__ == "__main__":
    paths = sources()
    print("Sources ready: " + ", ".join(str(p) for p in paths), flush=True)
    if "--sources-only" not in sys.argv:
        subprocess.run([sys.executable, "-m", "venv", str(ROOT / ".deps" / "connectors-env")], check=True)
        python = ROOT / ".deps" / "connectors-env" / "Scripts" / "python.exe"
        subprocess.run([str(python), "-m", "pip", "install", *map(str, paths)], check=True)
        cfgpath = ROOT / "config.local.json"
        if not cfgpath.exists():
            cfgpath.write_text(json.dumps({"xxt_command": [str(python), str(ROOT / "scripts" / "xxt_readonly.py")],
                                          "xxt_cwd": str(paths[0]), "zhs_python": str(python),
                                          "sync_interval_minutes": 30}, ensure_ascii=False, indent=2), encoding="utf-8")

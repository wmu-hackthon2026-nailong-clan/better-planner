"""Install the project's pinned connectors with an official, local Python runtime."""
import json
import subprocess
import sys
import zipfile
from pathlib import Path

from prepare_connectors import ROOT, download, sources


def main():
    cache = ROOT / ".deps"
    cache.mkdir(exist_ok=True)
    runtime = ROOT / "runtime"
    runtime.mkdir(exist_ok=True)
    archive = cache / "python-3.14.0-embed-amd64.zip"
    download("https://www.python.org/ftp/python/3.14.0/python-3.14.0-embed-amd64.zip", archive)
    python = runtime / "python.exe"
    if not python.exists():
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                destination = (runtime / item.filename).resolve()
                if not destination.is_relative_to(runtime.resolve()):
                    raise RuntimeError("Unsafe archive entry")
            source.extractall(runtime)
    (runtime / "python314._pth").write_text("python314.zip\n.\nLib/site-packages\n..\nimport site\n", encoding="utf-8")
    if subprocess.run([str(python), "-m", "pip", "--version"], capture_output=True).returncode:
        bootstrap = cache / "get-pip.py"
        download("https://bootstrap.pypa.io/get-pip.py", bootstrap)
        subprocess.run([str(python), str(bootstrap), "--no-warn-script-location"], check=True)
    paths = sources()
    subprocess.run([str(python), "-m", "pip", "install", "--no-warn-script-location", "setuptools>=68", "wheel", "hatchling"], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "--no-build-isolation", "--no-warn-script-location", *map(str, paths)], check=True)
    subprocess.run([str(python), "-c", "import xuexitong_mcp.client, zhs.login; print('Connectors ready')"], check=True)
    path = ROOT / "config.local.json"
    cfg = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}
    cfg.setdefault("desktop_python", str(python))
    cfg.setdefault("xxt_command", [str(python), str(ROOT / "scripts" / "xxt_readonly.py")])
    cfg.setdefault("xxt_cwd", str(ROOT))
    cfg.setdefault("zhs_python", str(python))
    cfg.setdefault("auth_python", str(python))
    cfg.setdefault("sync_interval_minutes", 30)
    path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Ready. Open Better Planner and click the account/sync button.", flush=True)


if __name__ == "__main__":
    main()

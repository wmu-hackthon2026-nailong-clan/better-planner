"""Build a Windows x64 offline ZIP from an official embedded runtime and installed connectors.

Only whitelisted application files are copied. User data and local configuration never enter the ZIP.
"""
import hashlib
import importlib.metadata
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / ".deps"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_connectors import REVISIONS


def build():
    runtime_zip = DEPS / "python-3.14.0-embed-amd64.zip"
    packages = DEPS / "site-packages"
    if not runtime_zip.exists() or not (packages / "zhs").is_dir() or not (packages / "xuexitong_mcp").is_dir():
        raise RuntimeError("Prepare the official Python runtime ZIP and connector dependencies first")
    target = ROOT / "dist" / "Keqi-Windows-x64"
    if target.exists() and "--refresh" not in sys.argv:
        raise RuntimeError("Build output already exists; use a fresh output directory to avoid mixing versions")
    target.mkdir(parents=True, exist_ok=True)
    for folder in ("app", "web", "examples"):
        shutil.copytree(ROOT / folder, target / folder, dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    scripts = target / "scripts"
    scripts.mkdir(exist_ok=True)
    for name in ("auth_worker.py", "xxt_readonly.py", "zhs_readonly.py", "enable_calendar_lan.ps1"):
        shutil.copy2(ROOT / "scripts" / name, scripts / name)
    shutil.copy2(ROOT / "launch.py", target / "launch.py")
    relay = DEPS / 'cloudflared'
    if relay.exists():
        relay_tools = target / 'tools'
        relay_tools.mkdir(exist_ok=True)
        relay_manifest = json.loads((relay / 'manifest.json').read_text())
        executable = relay / 'cloudflared-windows-amd64.exe'
        if hashlib.sha256(executable.read_bytes()).hexdigest() != relay_manifest['sha256']:
            raise RuntimeError('cloudflared checksum mismatch')
        shutil.copy2(executable, relay_tools / 'cloudflared.exe')
        shutil.copy2(relay / 'manifest.json', relay_tools / 'cloudflared-manifest.json')
        shutil.copy2(relay / 'LICENSE', relay_tools / 'cloudflared-LICENSE.txt')
    runtime = target / "runtime"
    with zipfile.ZipFile(runtime_zip) as z:
        z.extractall(runtime)
    (runtime / "python314._pth").write_text("python314.zip\n.\nsite-packages\n..\nimport site\n", encoding="utf-8")
    shutil.copytree(packages, runtime / "site-packages", dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    # Batch is ASCII so the launcher works regardless of Windows console code page.
    (target / "Start-Keqi.bat").write_bytes(
        b'@echo off\r\ncd /d "%~dp0"\r\n"%~dp0runtime\\python.exe" "%~dp0launch.py"\r\n'
        b'if errorlevel 1 (echo Failed to start Keqi. Please keep the complete folder and try again. & pause)\r\n')
    (target / "Enable-Calendar-LAN.bat").write_bytes(
        b'@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\\enable_calendar_lan.ps1"\r\n'
        b'if errorlevel 1 (echo Firewall setup failed. & pause)\r\n')
    (target / "使用说明.txt").write_text(
        "课期 Windows x64 便携版\n\n1. 将整个 ZIP 解压到可写目录。\n"
        "2. 双击 Start-Keqi.bat，浏览器会自动打开本地看板。\n"
        "3. 点击「连接账号」：学习通输入账号密码；智慧树使用知到 App 扫码确认。\n"
        "4. 点击「同步平台」，在「数据连接」查看结果。\n\n"
        "手机日历：点击「手机日历订阅」，复制链接到一加日历的 URL 导入。手机和电脑需在同一局域网。\n"
        "若手机无法打开链接，双击 Enable-Calendar-LAN.bat 并确认 Windows 管理员提示，只放行局域网日历端口 8768。\n"
        "无需安装 Git 或 Python。运行时请保留命令窗口，关闭窗口即停止后台服务。\n"
        "数据与登录态保存在 data/，请勿把使用后的整个文件夹直接发给别人；分享原始 ZIP。\n"
        "学习通覆盖当前周和下周课表、作业、考试；智慧树覆盖知到共享课。AI 课和翻转课请手动补录。\n"
        "登录接口可能要求额外验证，真实账号尚需用户验证。\n"
        "依赖源码与许可证见 sources/ 和 THIRD_PARTY_NOTICES.txt。\n", encoding="utf-8-sig")
    sources = target / "sources"
    sources.mkdir(exist_ok=True)
    for name, (repo, rev) in REVISIONS.items():
        sourcepath = DEPS / name
        actual = subprocess.check_output(["git", "-C", str(sourcepath), "rev-parse", "HEAD"], text=True).strip()
        if actual != rev:
            raise RuntimeError(f"Unexpected source revision for {name}")
        subprocess.run(["git", "-C", str(sourcepath), "archive", "--format=zip", "--output", str(sources / (name + ".zip")), rev], check=True)
    (target / "THIRD_PARTY_NOTICES.txt").write_text(
        "Python 3.14.0: Python Software Foundation License; see runtime/LICENSE.txt\n"
        "xuexitong-cli: DanMo661 / Wu Jiale, MIT; source and license in sources/xuexitong.zip\n"
        "zhs: pjm314159, GPL-3.0-only; corresponding source and license in sources/zhs.zip\n"
        "Python distributions: see runtime/site-packages/*.dist-info, including license metadata.\n"
        "cloudflared: Cloudflare, Apache-2.0; see tools/cloudflared-LICENSE.txt and cloudflared-manifest.json.\n"
        "No third-party source has been modified by this bundle; platform adapters run in separate processes.\n",
        encoding="utf-8")
    manifest = dict(app="Keqi", version="0.3.1", platform="Windows x64", python="3.14.0",
                    python_archive_sha256=hashlib.sha256(runtime_zip.read_bytes()).hexdigest(),
                    connectors={k: dict(repository=v[0], revision=v[1]) for k, v in REVISIONS.items()},
                    dependencies={d.metadata["Name"]: d.version for d in importlib.metadata.distributions(path=[str(packages)])})
    (target / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    # Isolated embedded runtime must import the application and both third-party clients.
    subprocess.run([str(runtime / "python.exe"), "-c",
                    "import app.server,sqlite3,ssl; from xuexitong_mcp.client import ChaoxingClient; "
                    "from zhs.login import LoginManager; from zhs.session import ZhsSession; print('Portable imports OK')"],
                   cwd=target, check=True)
    archive = ROOT / "dist" / "Keqi-Windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in target.rglob("*"):
            if path.is_file() and "__pycache__" not in path.parts:
                rel = path.relative_to(target)
                if rel.parts[0] not in ("app", "web", "examples", "scripts", "runtime", "sources", "tools",
                                       "launch.py", "Start-Keqi.bat", "Enable-Calendar-LAN.bat",
                                       "使用说明.txt", "THIRD_PARTY_NOTICES.txt", "build-manifest.json"):
                    continue
                z.write(path, "Keqi-Windows-x64/" + rel.as_posix())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(digest + "  " + archive.name + "\n", encoding="ascii")
    print(json.dumps(dict(archive=str(archive), size_mb=round(archive.stat().st_size/1024/1024, 1), sha256=digest)), flush=True)


if __name__ == "__main__":
    build()

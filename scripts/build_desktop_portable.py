"""Build a Windows x64 offline ZIP for the C++ desktop app (better_planner.exe).

Packs the desktop executable, the MSVC runtime DLLs it links against, the
official embedded Python runtime, and the connector scripts it shells out to
for platform sync. User data and local config never enter the ZIP.
"""
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / ".deps"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_connectors import REVISIONS  # noqa: E402

# VC++ redistributable CRT DLLs the exe links (built with /MD).
MSVC_REDIST = Path("D:/VisualStudioPackages/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT")
CRT_DLLS = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")

# Application files the desktop app needs at runtime (relative to project root).
APP_FILES = (
    "better_planner.exe",  # staged from build/
    "config.local.json",   # generated with the embedded runtime path
)
APP_DIRS = (
    "app",       # desktop_service + auth/connectors/core
    "scripts",   # connector scripts (xxt/zhs read-only bridges)
)


def stage_exe(target: Path) -> None:
    src = ROOT / "build" / "better_planner.exe"
    if not src.exists():
        raise RuntimeError("先构建桌面版：powershell -ExecutionPolicy Bypass -File scripts\\build.ps1")
    shutil.copy2(src, target / "better_planner.exe")


def stage_crt(target: Path) -> None:
    if not MSVC_REDIST.is_dir():
        raise RuntimeError(f"未找到 MSVC 运行时目录：{MSVC_REDIST}")
    for name in CRT_DLLS:
        p = MSVC_REDIST / name
        if not p.exists():
            raise RuntimeError(f"缺少运行时 DLL：{p}")
        shutil.copy2(p, target / name)


def stage_config(target: Path) -> None:
    """Write a config.local.json pointing at the bundled embedded Python."""
    py = (target / "runtime" / "python.exe").resolve()
    cfg = {
        "desktop_python": str(py),
        "xxt_command": [str(py), str((target / "scripts" / "xxt_readonly.py").resolve())],
        "xxt_cwd": str(target.resolve()),
        "zhs_python": str(py),
        "sync_interval_minutes": 30,
    }
    # 沿用开发者本地已配置的学期开始日期（课表周次按学期第 1 周展开）。
    local_cfg = ROOT / "config.local.json"
    if local_cfg.exists():
        try:
            local = json.loads(local_cfg.read_text(encoding="utf-8-sig"))
            if "semester_start" in local:
                cfg["semester_start"] = local["semester_start"]
        except (json.JSONDecodeError, OSError):
            pass
    (target / "config.local.json").write_text(
        json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")


def build() -> None:
    runtime_zip = DEPS / "python-3.14.0-embed-amd64.zip"
    # 连接器已安装到嵌入式运行时里，直接复用，无需重新安装。
    packages = ROOT / "runtime" / "Lib" / "site-packages"
    if not runtime_zip.exists() or not (packages / "zhs").is_dir() or not (packages / "xuexitong_mcp").is_dir():
        raise RuntimeError("先运行 python scripts/setup_desktop.py 准备运行时与连接器依赖")

    target = ROOT / "dist" / "Keqi-Desktop-Windows-x64"
    if target.exists() and "--refresh" not in sys.argv:
        raise RuntimeError("打包目录已存在；请删除或加 --refresh")
    target.mkdir(parents=True, exist_ok=True)

    # 1) 主程序 + 运行时 DLL + 配置
    stage_exe(target)
    stage_crt(target)
    stage_config(target)

    # 2) 应用目录（只复制白名单文件，排除缓存）
    for folder in APP_DIRS:
        shutil.copytree(
            ROOT / folder, target / folder, dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    # 3) 嵌入式 Python 运行时 + 连接器
    runtime = target / "runtime"
    with zipfile.ZipFile(runtime_zip) as z:
        z.extractall(runtime)
    (runtime / "python314._pth").write_text(
        "python314.zip\n.\nLib/site-packages\n..\nimport site\n", encoding="utf-8")
    shutil.copytree(packages, runtime / "Lib" / "site-packages",
                    dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    # 4) 启动脚本（ASCII 批处理，避免代码页问题）
    (target / "Start-Keqi.bat").write_bytes(
        b'@echo off\r\ncd /d "%~dp0"\r\nstart "" "%~dp0better_planner.exe"\r\n'
        b'if errorlevel 1 (echo Failed to start Keqi. & pause)\r\n')

    # 5) 使用说明
    (target / "使用说明.txt").write_text(
        "课期 Windows x64 桌面版\n\n"
        "1. 将整个 ZIP 解压到可写目录（保持完整文件夹，勿单独拖出 exe）。\n"
        "2. 双击 Start-Keqi.bat 启动桌面应用。\n"
        "3. 首次使用：工具栏「连接与同步」打开本机账号页，学习通账号密码登录、智慧树扫码登录。\n"
        "4. 点「连接与同步」同步平台；点「导入课表」导入教务系统课表 JSON。\n\n"
        "数据保存在本目录 data/ 下；请勿把使用后的文件夹直接发给别人，分享原始 ZIP。\n"
        "若启动报缺少 DLL，请安装微软 VC++ 2015-2022 运行库（vcredist_x64.exe）。\n"
        "依赖源码与许可证见 sources/ 和 THIRD_PARTY_NOTICES.txt。\n",
        encoding="utf-8-sig")

    # 6) 第三方依赖源码与许可证（直接复用已下载的归档，无需 git）
    sources = target / "sources"
    sources.mkdir(exist_ok=True)
    for name, (_repo, rev) in REVISIONS.items():
        archive = DEPS / f"{name}-{rev}.zip"
        if not archive.exists():
            raise RuntimeError(f"缺少连接器源码归档：{archive}（先运行 setup_desktop.py）")
        shutil.copy2(archive, sources / f"{name}.zip")
        license_src = DEPS / name / "LICENSE"
        if license_src.exists():
            shutil.copy2(license_src, sources / f"{name}-LICENSE.txt")
    (target / "THIRD_PARTY_NOTICES.txt").write_text(
        "Python 3.14.0: Python Software Foundation License\n"
        "xuexitong-cli: MIT; source in sources/xuexitong.zip\n"
        "zhs: pjm314159, GPL-3.0-only; source in sources/zhs.zip\n"
        "Microsoft Visual C++ Runtime: Microsoft Software License Terms\n",
        encoding="utf-8")

    # 7) 验证打包内的嵌入式运行时能导入连接器
    subprocess.run(
        [str(runtime / "python.exe"), "-c",
         "import sqlite3,ssl; from xuexitong_mcp.client import ChaoxingClient; "
         "from zhs.login import LoginManager; from zhs.session import ZhsSession; print('OK')"],
        cwd=target, check=True)

    # 8) 打包 zip + 校验
    archive = ROOT / "dist" / "Keqi-Desktop-Windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(target.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                z.write(path, "Keqi-Desktop-Windows-x64/" + path.relative_to(target).as_posix())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(
        digest + "  " + archive.name + "\n", encoding="ascii")
    print(json.dumps(dict(archive=str(archive),
                          size_mb=round(archive.stat().st_size / 1024 / 1024, 1),
                          sha256=digest), ensure_ascii=False), flush=True)


if __name__ == "__main__":
    build()

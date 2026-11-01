"""One-shot login process; secrets arrive through stdin, never command arguments."""
import base64
import contextlib
import json
import os
import sys

OUTPUT = sys.stdout


def emit(status, message, **extra):
    print(json.dumps(dict(status=status, message=message, **extra), ensure_ascii=False), file=OUTPUT, flush=True)


def login(provider, data):
    if provider == "chaoxing":
        os.environ["CHAOXING_PHONE"] = data["phone"].strip()
        os.environ["CHAOXING_PASSWORD"] = data["password"]
        from xuexitong_mcp.client import ChaoxingClient
        from pathlib import Path
        directory = Path(__file__).resolve().parent.parent / "data" / "xxt"
        directory.mkdir(parents=True, exist_ok=True)
        client = ChaoxingClient(config_dir=str(directory))
        result = client.login()
        if not result.ok:
            emit("error", "学习通登录失败，请检查账号密码；若出现验证码，请先在官方客户端完成验证。")
            return
        emit("success", "学习通已连接，可以同步课程和任务。")
    else:
        from zhs.config import ConfigManager
        from zhs.session import ZhsSession
        from zhs.login import LoginManager
        from zhs.utils.cookie import cookies_to_list
        from zhs.utils.path import get_data_dir
        cfg = ConfigManager().load()
        session = ZhsSession(cfg)
        try:
            manager = LoginManager(session, cfg)
            def qr(image):
                emit("qr", "请打开知到 App 扫描二维码并确认登录。", image=base64.b64encode(image).decode())
            result = manager.login_with_qr(qr)
            if not result.success:
                emit("error", "扫码未完成，请重新获取二维码。")
                return
            if not result.cookies:
                emit("error", "平台未返回登录态，请重新扫码。")
                return
            path = get_data_dir() / "cookies.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(cookies_to_list(result.cookies)), encoding="utf-8")
            emit("success", "智慧树已连接，可以同步知到共享课任务。")
        finally:
            session.close()


if __name__ == "__main__":
    try:
        body = json.loads(sys.stdin.readline())
        with contextlib.redirect_stdout(sys.stderr):
            login(sys.argv[1], body)
    except ImportError:
        emit("error", "缺少平台连接器，请使用完整启动包或运行连接器安装脚本。")
    except Exception:
        emit("error", "登录失败，请检查网络与账号，或重新扫码。")

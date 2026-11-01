"""Portable entry point. Launch a server owned by this user and open its page."""
import json
import sys
import threading
import urllib.error
import urllib.request
import webbrowser

from app.server import main


if __name__ == "__main__":
    port = 8765
    url = f"http://127.0.0.1:{port}"
    try:
        with urllib.request.urlopen(url + "/api/state", timeout=2) as response:
            state = json.load(response)
        if state.get("app") != "keqi":
            raise ValueError("Port in use by another application")
        webbrowser.open(url)
    except (OSError, ValueError, urllib.error.URLError):
        from socket import socket
        with socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                port = 8766
        url = f"http://127.0.0.1:{port}"
        sys.argv = [sys.argv[0], "--port", str(port), "--auto-sync"]
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()
        main()

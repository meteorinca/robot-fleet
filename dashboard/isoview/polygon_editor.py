#!/usr/bin/env python3
"""
IsoView Polygon Studio - Local WebUI Editor
Run: python polygon_editor.py [--port 5050]
"""

import os
import sys
import json
import shutil
import argparse
import webbrowser
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
ROOMS_FILE = BASE_DIR / "rooms.json"
BACKUP_ROOMS_FILE = BASE_DIR / "rooms.json.bak"
BOTS_FILE = BASE_DIR / "bots.json"
BACKUP_BOTS_FILE = BASE_DIR / "bots.json.bak"
HTML_FILE = BASE_DIR / "editor.html"

def load_rooms():
    if not ROOMS_FILE.exists():
        return {"image": {"w": 1189, "h": 1323}, "rooms": []}
    try:
        with open(ROOMS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print(f"[Error] Failed reading rooms.json: {e}")
        return {"image": {"w": 1189, "h": 1323}, "rooms": []}

def save_rooms(data):
    if ROOMS_FILE.exists():
        try:
            shutil.copyfile(ROOMS_FILE, BACKUP_ROOMS_FILE)
        except Exception:
            pass

    with open(ROOMS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return True

def load_bots():
    if not BOTS_FILE.exists():
        return {"bots": []}
    try:
        with open(BOTS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print(f"[Error] Failed reading bots.json: {e}")
        return {"bots": []}

def save_bots(data):
    if BOTS_FILE.exists():
        try:
            shutil.copyfile(BOTS_FILE, BACKUP_BOTS_FILE)
        except Exception:
            pass

    with open(BOTS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return True

def run_flask_server(port=5050, open_browser=True, open_dashboard=False):
    from flask import Flask, request, jsonify, send_from_directory

    app = Flask(__name__, static_folder=str(BASE_DIR))

    @app.route("/")
    def index():
        return send_from_directory(str(BASE_DIR), "editor.html")

    @app.route("/dashboard")
    def dashboard():
        return send_from_directory(str(BASE_DIR), "index.html")

    @app.route("/<path:filename>")
    def static_files(filename):
        return send_from_directory(str(BASE_DIR), filename)

    @app.route("/api/rooms", methods=["GET"])
    def get_rooms():
        return jsonify(load_rooms())

    @app.route("/api/save", methods=["POST"])
    def post_save():
        try:
            payload = request.get_json(force=True)
            if not payload or "rooms" not in payload:
                return jsonify({"status": "error", "message": "Invalid JSON format"}), 400
            save_rooms(payload)
            print(f"[Saved] Successfully updated {ROOMS_FILE} ({len(payload['rooms'])} rooms)")
            return jsonify({"status": "ok", "message": "Saved successfully!"})
        except Exception as e:
            return jsonify({"status": "error", "message": str(e)}), 500

    @app.route("/api/bots", methods=["GET"])
    def get_bots():
        return jsonify(load_bots())

    @app.route("/api/save_bots", methods=["POST"])
    def post_save_bots():
        try:
            payload = request.get_json(force=True)
            if not payload or "bots" not in payload:
                return jsonify({"status": "error", "message": "Invalid JSON format"}), 400
            save_bots(payload)
            print(f"[Saved] Successfully updated {BOTS_FILE} ({len(payload['bots'])} bots)")
            return jsonify({"status": "ok", "message": "Saved bots successfully!"})
        except Exception as e:
            return jsonify({"status": "error", "message": str(e)}), 500

    editor_url = f"http://localhost:{port}"
    dash_url = f"http://localhost:{port}/index.html"
    print("=" * 64)
    print(f"  IsoView Polygon Studio : {editor_url}")
    print(f"  IsoView Main Dashboard : {dash_url}")
    print("=" * 64)

    if open_browser:
        target = dash_url if open_dashboard else editor_url
        webbrowser.open(target)

    app.run(host="127.0.0.1", port=port, debug=False)

def run_stdlib_server(port=5050, open_browser=True):
    from http.server import HTTPServer, SimpleHTTPRequestHandler
    from urllib.parse import urlparse

    class PolygonHandler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(BASE_DIR), **kwargs)

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path in ("/", "/index.html"):
                self.path = "/editor.html"
                return super().do_GET()
            elif parsed.path == "/api/rooms":
                data = json.dumps(load_rooms()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            elif parsed.path == "/api/bots":
                data = json.dumps(load_bots()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            return super().do_GET()

        def do_POST(self):
            parsed = urlparse(self.path)
            if parsed.path == "/api/save":
                content_len = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_len)
                try:
                    payload = json.loads(body.decode("utf-8"))
                    save_rooms(payload)
                    resp = json.dumps({"status": "ok"}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(resp)))
                    self.end_headers()
                    self.wfile.write(resp)
                    print(f"[Saved] Successfully updated {ROOMS_FILE}")
                except Exception as e:
                    err = json.dumps({"status": "error", "message": str(e)}).encode("utf-8")
                    self.send_response(500)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(err)))
                    self.end_headers()
                    self.wfile.write(err)
                return
            elif parsed.path == "/api/save_bots":
                content_len = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_len)
                try:
                    payload = json.loads(body.decode("utf-8"))
                    save_bots(payload)
                    resp = json.dumps({"status": "ok"}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(resp)))
                    self.end_headers()
                    self.wfile.write(resp)
                    print(f"[Saved] Successfully updated {BOTS_FILE}")
                except Exception as e:
                    err = json.dumps({"status": "error", "message": str(e)}).encode("utf-8")
                    self.send_response(500)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(err)))
                    self.end_headers()
                    self.wfile.write(err)
                return
            self.send_response(404)
            self.end_headers()

    url = f"http://localhost:{port}"
    print("=" * 64)
    print(f"  IsoView Polygon Studio running at: {url}")
    print("=" * 64)

    if open_browser:
        webbrowser.open(url)

    server = HTTPServer(("127.0.0.1", port), PolygonHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server.")

def main():
    parser = argparse.ArgumentParser(description="IsoView Polygon Studio & Dashboard")
    parser.add_argument("--port", type=int, default=5050, help="Server port (default: 5050)")
    parser.add_argument("--no-browser", action="store_true", help="Do not automatically open browser")
    parser.add_argument("--dashboard", action="store_true", help="Open main IsoView dashboard instead of polygon editor")
    args = parser.parse_args()

    try:
        import flask
        run_flask_server(port=args.port, open_browser=not args.no_browser, open_dashboard=args.dashboard)
    except ImportError:
        run_stdlib_server(port=args.port, open_browser=not args.no_browser)

if __name__ == "__main__":
    main()

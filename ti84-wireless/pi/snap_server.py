"""Camera -> Gemini -> TI-84 bridge, running on the Raspberry Pi.

The C3 firmware's SNAP command POSTs the calculator's prompt here; this takes a
still with the Pi camera, asks Gemini about it and returns plain text, which the
C3 folds to ASCII and wraps for the calculator screen.

    POST /snap      body: prompt text (may be empty)  ->  200 text/plain answer
    GET  /health    ->  200 "ok"

Config (environment, or ~/ti84-snap/.env as KEY=value lines):
    GEMINI_API_KEY   required
    GEMINI_MODEL     default gemini-flash-latest
    SNAP_PORT        default 8084
    SNAP_TEST_IMAGE  path to a JPEG to use instead of the camera (testing)

Standard library only, plus picamera2 from Raspberry Pi OS.
"""
import base64
import io
import json
import os
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ENV_FILE = Path.home() / "ti84-snap" / ".env"
FALLBACK_MODEL = "gemini-flash-lite-latest"
DEFAULT_PROMPT = "Read this and answer it. If it is a problem, solve it and show brief steps."
SYSTEM = (
    "You answer on a TI-84 calculator screen (26x10 characters) about a photo the user took. "
    "Reply in plain ASCII text only: no Markdown, no LaTeX, no emoji, no tables. "
    "Be brief: a few short lines unless asked for more. "
    "Write math inline, e.g. x^2+3x-4=0, sqrt(2), pi. "
    "If the photo is unreadable, say so in one line."
)
MAX_WIDTH = 1600  # px; plenty for text, keeps the upload small over a phone hotspot


def load_env():
    if ENV_FILE.exists():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


class Camera:
    """Keeps picamera2 open between shots; first capture after start takes ~1 s."""

    def __init__(self):
        self.lock = threading.Lock()
        self.cam = None

    def jpeg(self):
        test = os.environ.get("SNAP_TEST_IMAGE")
        if test:
            return Path(test).read_bytes()
        with self.lock:
            if self.cam is None:
                from picamera2 import Picamera2  # imported late so tests run without a camera

                cam = Picamera2()
                cam.configure(cam.create_still_configuration(main={"size": (2304, 1296)}))
                cam.start()
                time.sleep(1.0)  # let auto exposure / focus settle
                self.cam = cam
            img = self.cam.capture_image("main")
        if img.width > MAX_WIDTH:
            img = img.resize((MAX_WIDTH, img.height * MAX_WIDTH // img.width))
        buf = io.BytesIO()
        img.convert("RGB").save(buf, "JPEG", quality=85)
        return buf.getvalue()


def gemini(model, key, prompt, jpeg):
    body = {
        "systemInstruction": {"parts": [{"text": SYSTEM}]},
        "contents": [{
            "role": "user",
            "parts": [
                {"inline_data": {"mime_type": "image/jpeg", "data": base64.b64encode(jpeg).decode()}},
                {"text": prompt},
            ],
        }],
        "generationConfig": {"maxOutputTokens": 2048},
    }
    req = urllib.request.Request(
        f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "x-goog-api-key": key},
    )
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.load(e)
        except ValueError:
            return e.code, {}
    except (TimeoutError, urllib.error.URLError) as e:
        return 0, {"error": {"message": f"Gemini unreachable: {getattr(e, 'reason', e)}"}}


def answer(prompt, jpeg):
    key = os.environ.get("GEMINI_API_KEY", "")
    if not key:
        raise RuntimeError("no GEMINI_API_KEY on the Pi")
    model = os.environ.get("GEMINI_MODEL", "gemini-flash-latest")
    code, data = gemini(model, key, prompt, jpeg)
    # Busy, rate-limited or hung: one retry on the lighter model.
    if code in (0, 429, 503) and model != FALLBACK_MODEL:
        code, data = gemini(FALLBACK_MODEL, key, prompt, jpeg)
    if code != 200:
        raise RuntimeError((data.get("error", {}).get("message") or f"HTTP {code}")[:200])
    parts = data.get("candidates", [{}])[0].get("content", {}).get("parts", [])
    text = "".join(p.get("text", "") for p in parts).strip()
    if not text:
        reason = data.get("candidates", [{}])[0].get("finishReason", "empty")
        raise RuntimeError(f"no answer ({reason})")
    return text


camera = Camera()


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, text):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, "ok")
        else:
            self.reply(404, "not found")

    def do_POST(self):
        if self.path != "/snap":
            self.reply(404, "not found")
            return
        n = int(self.headers.get("Content-Length") or 0)
        prompt = self.rfile.read(n).decode(errors="replace").strip() or DEFAULT_PROMPT
        t0 = time.time()
        try:
            jpeg = camera.jpeg()
            text = answer(prompt, jpeg)
            self.reply(200, text)
            self.log_message("snap ok in %.1fs (%d KB image)", time.time() - t0, len(jpeg) // 1024)
        except Exception as e:  # report every failure to the calculator as text
            self.reply(500, str(e) or e.__class__.__name__)
            self.log_message("snap failed: %s", e)


def main():
    load_env()
    port = int(os.environ.get("SNAP_PORT", "8084"))
    print(f"ti84-snap listening on :{port}", flush=True)
    ThreadingHTTPServer(("", port), Handler).serve_forever()


if __name__ == "__main__":
    main()

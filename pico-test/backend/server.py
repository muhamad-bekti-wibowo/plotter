"""Backend Python untuk Kontrol Motor Pico.

Tugas backend: serial ke Pico (pyserial), konversi SVG -> goresan (svgelements + numpy, jauh lebih cepat
daripada di browser), dan streaming gambar ke Pico dengan antrean berjalan (tanpa menunggu "ok" tiap baris).
Halaman web (web/index.html) hanya tampilan; semua dilayani dari http://127.0.0.1:8765
"""
import hashlib
import io
import json
import math
import queue
import re
import threading
import time
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
import serial
import serial.tools.list_ports
from svgelements import SVG, Arc, CubicBezier, Move, QuadraticBezier, Shape
from svgelements import Path as SvgPath

HOST, PORT = "127.0.0.1", 8765
WEB = Path(__file__).parent / "web"
PICO_QUEUE = 100          # MAXLEG di firmware
QUEUE_MARGIN = 12         # sisakan slot supaya Pico tidak pernah menjawab "full"


# ---------------------------------------------------------------- serial
class Link:
    """Satu koneksi serial; semua baris masuk disebar ke setiap pelanggan (browser lewat SSE, Drawer)."""

    def __init__(self):
        self.ser = None
        self.lock = threading.Lock()
        self.subs = set()

    def subscribe(self):
        q = queue.Queue(maxsize=5000)
        self.subs.add(q)
        return q

    def unsubscribe(self, q):
        self.subs.discard(q)

    def publish(self, ev):
        for q in list(self.subs):
            try:
                q.put_nowait(ev)
            except queue.Full:
                pass

    @property
    def connected(self):
        return self.ser is not None and self.ser.is_open

    def open(self, port):
        self.close()
        self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=2)
        threading.Thread(target=self._reader, args=(self.ser,), daemon=True).start()
        self.publish({"t": "conn", "on": True, "port": port})

    def close(self):
        s, self.ser = self.ser, None
        if s:
            try:
                s.close()
            except Exception:
                pass
            self.publish({"t": "conn", "on": False})

    def write_lines(self, lines):
        if not self.connected:
            raise IOError("tidak terhubung")
        with self.lock:
            self.ser.write(("\n".join(lines) + "\n").encode())

    def _reader(self, ser):
        buf = b""
        try:
            while ser is self.ser and ser.is_open:
                chunk = ser.read(ser.in_waiting or 1)
                if not chunk:
                    continue
                buf += chunk
                while True:
                    m = re.search(rb"[\r\n]", buf)
                    if not m:
                        break
                    line, buf = buf[: m.start()], buf[m.end():]
                    if line.strip():
                        self.publish({"t": "line", "d": line.decode(errors="replace").strip()})
        except Exception as e:
            self.publish({"t": "line", "d": f"error: serial terputus ({e})"})
        if ser is self.ser:
            self.close()


link = Link()


# ---------------------------------------------------------------- SVG -> goresan
_cache = {"key": None, "polys": None, "bbox": None}
_last = {"strokes": [], "w": 0.0, "h": 0.0}


def _hidden(e):
    v = getattr(e, "values", None) or {}
    style = str(v.get("style", ""))
    return v.get("display") == "none" or v.get("visibility") == "hidden" or bool(re.search(r"display\s*:\s*none", style))


def _curve_len(seg):
    pts = [seg.start]
    if isinstance(seg, CubicBezier):
        pts += [seg.control1, seg.control2]
    elif isinstance(seg, QuadraticBezier):
        pts += [seg.control]
    pts.append(seg.end)
    return sum(math.hypot(b.x - a.x, b.y - a.y) for a, b in zip(pts, pts[1:]))


def sample_svg(text):
    """Baca SVG, kembalikan list polyline (array Nx2, koordinat SVG) dan bbox."""
    svg = SVG.parse(io.StringIO(text))
    bb = svg.bbox()
    diag = math.hypot(bb[2] - bb[0], bb[3] - bb[1]) if bb else 1000.0
    step = max(diag / 4000.0, 1e-3)
    polys = []
    for e in svg.elements():
        if not isinstance(e, Shape) or _hidden(e):
            continue
        p = SvgPath(e)
        p.reify()
        for sub in p.as_subpaths():
            pts = []
            for seg in SvgPath(sub):
                if isinstance(seg, Move):
                    pts.append([seg.end.x, seg.end.y])
                    continue
                if isinstance(seg, (CubicBezier, QuadraticBezier)):
                    ln = _curve_len(seg)
                elif isinstance(seg, Arc):
                    ln = seg.length()
                else:
                    pts.append([seg.end.x, seg.end.y])
                    continue
                n = int(min(2000, max(2, math.ceil(ln / step))))
                pts.extend(seg.npoint(np.linspace(0, 1, n + 1))[1:].tolist())
            if len(pts) >= 2:
                polys.append(np.array(pts, dtype=float))
    if not polys:
        raise ValueError("Tidak ada bentuk yang bisa digambar di SVG ini")
    allp = np.vstack(polys)
    return polys, (allp[:, 0].min(), allp[:, 1].min(), allp[:, 0].max(), allp[:, 1].max())


def rdp(pts, tol):
    n = len(pts)
    if n < 3:
        return pts
    keep = np.zeros(n, bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        seg, A, B = pts[a + 1:b], pts[a], pts[b]
        d = B - A
        L = math.hypot(d[0], d[1])
        if L > 0:
            dist = np.abs(d[1] * (seg[:, 0] - A[0]) - d[0] * (seg[:, 1] - A[1])) / L
        else:
            dist = np.hypot(seg[:, 0] - A[0], seg[:, 1] - A[1])
        i = int(np.argmax(dist))
        if dist[i] > tol:
            m = a + 1 + i
            keep[m] = True
            stack += [(a, m), (m, b)]
    return pts[keep]


def order_strokes(strokes):
    """Jalur terdekat berikutnya (boleh dibalik) supaya perjalanan pena naik pendek."""
    if len(strokes) < 2:
        return strokes
    starts = np.array([s[0] for s in strokes])
    ends = np.array([s[-1] for s in strokes])
    alive = np.ones(len(strokes), bool)
    cur = np.zeros(2)
    out = []
    for _ in range(len(strokes)):
        d1 = np.where(alive, np.hypot(*(starts - cur).T), np.inf)
        d2 = np.where(alive, np.hypot(*(ends - cur).T), np.inf)
        i1, i2 = int(np.argmin(d1)), int(np.argmin(d2))
        if d1[i1] <= d2[i2]:
            i, s = i1, strokes[i1]
        else:
            i, s = i2, strokes[i2][::-1]
        alive[i] = False
        out.append(s)
        cur = s[-1]
    return out


def convert_svg(text, width_mm, tol_mm, optimize):
    key = hashlib.sha1(text.encode()).hexdigest()
    if _cache["key"] != key:
        _cache["polys"], _cache["bbox"] = sample_svg(text)
        _cache["key"] = key
    x0, y0, x1, y1 = _cache["bbox"]
    w, h = x1 - x0, y1 - y0
    s = width_mm / (w if w > 0 else (h or 1.0))
    strokes = []
    for p in _cache["polys"]:
        q = np.column_stack(((p[:, 0] - x0) * s, (y1 - p[:, 1]) * s))      # y dibalik: SVG ke bawah, plotter ke atas
        q = rdp(q, tol_mm)
        if len(q) >= 2 and np.hypot(*(q - q[0]).T).max() > 0.05:
            strokes.append(q)
    if optimize:
        strokes = order_strokes(strokes)
    _last.update(strokes=strokes, w=w * s, h=h * s)
    return {
        "strokes": [np.round(q, 3).tolist() for q in strokes],
        "w": w * s, "h": h * s,
    }


# ---------------------------------------------------------------- streaming gambar
class Drawer(threading.Thread):
    """Kirim perintah berurutan tanpa menunggu 'ok'; kedalaman antrean Pico dipantau lewat ST."""

    def __init__(self, strokes, ox, oy, dv):
        super().__init__(daemon=True)
        self.strokes, self.ox, self.oy, self.dv = strokes, ox, oy, dv
        self.stop_req = False
        self.state = "run"

    def _cmds(self):
        f = lambda v: f"{v:.3f}".rstrip("0").rstrip(".") or "0"
        cmds, ends = [f"DV {self.dv:g}"], []
        for p in self.strokes:
            cmds.append("PU")
            cmds.append(f"MV {f(p[0][0] + self.ox)} {f(p[0][1] + self.oy)}")
            cmds.append("PD")
            cmds += [f"MV {f(x + self.ox)} {f(y + self.oy)}" for x, y in p[1:]]
            cmds.append("PU")
            ends.append(len(cmds))
        return cmds, ends

    def _emit(self, done, total, state, msg=""):
        link.publish({"t": "draw", "state": state, "done": done, "total": total, "msg": msg})

    def run(self):
        cmds, ends = self._cmds()
        total = len(self.strokes)
        sub = link.subscribe()
        sent = base = qrep = 0
        pending = deque()
        last_st = last_ev = 0.0
        last_reply = time.time()
        busy = True
        idx = 0
        fail = ""
        done = 0
        try:
            self._emit(0, total, "run")
            while not self.stop_req:
                free = PICO_QUEUE - QUEUE_MARGIN - (qrep + sent - base)
                if idx < len(cmds) and free > 0:
                    batch = cmds[idx: idx + min(free, 24)]
                    link.write_lines(batch)
                    idx += len(batch)
                    sent += len(batch)
                now = time.time()
                if now - last_st > 0.1:
                    pending.append(sent)
                    link.write_lines(["ST"])
                    last_st = now
                try:
                    ev = sub.get(timeout=0.02)
                except queue.Empty:
                    ev = None
                while ev is not None:
                    if ev.get("t") == "conn" and not ev["on"]:
                        fail = "koneksi serial terputus"
                    elif ev.get("t") == "line":
                        d = ev["d"]
                        m = re.match(r"ST q=(\d+) busy=(\d)", d)
                        if m and pending:
                            qrep, busy, base = int(m[1]), m[2] == "1", pending.popleft()
                            last_reply = time.time()
                        elif d.startswith("limit"):
                            fail = d
                        elif d.startswith(("full", "stopped", "error")):
                            fail = f"Pico menjawab: {d}"
                    try:
                        ev = sub.get_nowait()
                    except queue.Empty:
                        ev = None
                if fail:
                    break
                if time.time() - last_reply > 5:
                    fail = "Pico tidak menjawab"
                    break
                consumed = max(0, base - qrep)                      # perkiraan perintah yang sudah keluar dari antrean Pico
                while done < total and ends[done] <= consumed:
                    done += 1
                if now - last_ev > 0.2:
                    self._emit(done, total, "run")
                    last_ev = now
                if idx >= len(cmds) and qrep == 0 and not busy and sent == base:
                    break
            if self.stop_req:
                link.write_lines(["STOP", "PEN UP"])
                self.state = "stopped"
                self._emit(done, total, "stopped", "Dihentikan")
            elif fail:
                try:
                    link.write_lines(["STOP", "PEN UP"])
                except Exception:
                    pass
                self.state = "error"
                self._emit(done, total, "error", fail)
            else:
                link.write_lines(["MV 0 0"])
                self.state = "done"
                self._emit(total, total, "done", "Selesai")
        except Exception as e:
            self.state = "error"
            self._emit(done, total, "error", str(e))
        finally:
            link.unsubscribe(sub)


drawer = None


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = (WEB / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/ports":
            ports = [{"device": p.device, "desc": p.description, "pico": p.vid == 0x2E8A}
                     for p in serial.tools.list_ports.comports()]
            self._json({"ports": ports, "connected": link.connected,
                        "port": link.ser.port if link.connected else None})
        elif self.path == "/api/events":
            self._events()
        else:
            self.send_error(404)

    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = link.subscribe()
        try:
            self.wfile.write(b": hi\n\n")
            while True:
                try:
                    ev = q.get(timeout=10)
                    self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except Exception:
            pass
        finally:
            link.unsubscribe(q)

    def do_POST(self):
        global drawer
        try:
            b = self._body()
            if self.path == "/api/connect":
                link.open(b["port"])
                self._json({"ok": True})
            elif self.path == "/api/disconnect":
                link.close()
                self._json({"ok": True})
            elif self.path == "/api/send":
                link.write_lines([b["cmd"]])
                self._json({"ok": True})
            elif self.path == "/api/svg":
                t0 = time.time()
                r = convert_svg(b["text"], float(b.get("width") or 40), float(b.get("tol") or 0.1), bool(b.get("opt", True)))
                r["ms"] = int((time.time() - t0) * 1000)
                self._json(r)
            elif self.path == "/api/draw":
                if drawer and drawer.is_alive():
                    return self._json({"ok": False, "error": "gambar sedang berjalan"})
                if not _last["strokes"]:
                    return self._json({"ok": False, "error": "belum ada SVG"})
                if not link.connected:
                    return self._json({"ok": False, "error": "tidak terhubung"})
                drawer = Drawer(_last["strokes"], float(b.get("ox") or 0), float(b.get("oy") or 0), float(b.get("dv") or 2))
                drawer.start()
                self._json({"ok": True})
            elif self.path == "/api/draw/stop":
                if drawer and drawer.is_alive():
                    drawer.stop_req = True
                self._json({"ok": True})
            else:
                self.send_error(404)
        except Exception as e:
            self._json({"ok": False, "error": str(e)}, 200)


def main():
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    srv.daemon_threads = True
    url = f"http://{HOST}:{PORT}"
    print(f"Kontrol Motor Pico berjalan di {url}  (Ctrl+C untuk berhenti)")
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    link.close()


if __name__ == "__main__":
    main()

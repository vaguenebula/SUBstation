#!/usr/bin/env python3
"""similarity_rater: rate sound triplets by ear, for the sound similarity's weights.

    python rate.py triplets.tsv [--ratings ratings.tsv] [--port 8765] [--seconds 2.5] [--names]

Serves a page on http://127.0.0.1:<port>/ that plays each triplet (A, B, C) of
triplets.tsv (written by sound_similarity_bench --triplets) and asks which of B
and C is more like A. Each answer is appended to ratings.tsv at once (next to
the triplets by default), so stopping and starting again carries on where it
was; sound_similarity_bench --ratings fits the weights to them
(benchmarks/README.md, Weights from listening).

--names shows the files' names (to listen to the misses sound_similarity_bench
--misses writes, say; not while rating, where a name could sway the answer).

Python 3.8 or newer, nothing else. The page only plays files named in the
triplets, and only listens on this computer.
"""

import argparse
import http.server
import json
import mimetypes
import os
import sys
import time
import webbrowser

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Similarity Rater</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --card: #fff; --line: #d9d9d4; --accent: #2f6fde; --on: #e8f0fd; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #18181a; --fg: #ececea; --muted: #9a9a95; --card: #232326; --line: #38383c; --accent: #6ea0ff; --on: #24324d; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 720px; margin: 0 auto; padding: 32px 16px; }
h1 { font-size: 18px; margin: 0 0 4px; }
.sub { color: var(--muted); margin: 0 0 24px; }
.row { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 12px; }
button { font: inherit; color: inherit; background: var(--card); border: 1px solid var(--line); border-radius: 10px;
         padding: 18px 8px; cursor: pointer; }
button:hover { border-color: var(--accent); }
.sound { font-size: 22px; font-weight: 600; }
.sound small { display: block; font-size: 12px; font-weight: 400; color: var(--muted); }
.sound.playing { background: var(--on); border-color: var(--accent); }
.answers { margin-top: 20px; grid-template-columns: 1fr 1fr; }
.answers .skip { grid-column: 1 / -1; padding: 10px; color: var(--muted); }
.status { margin-top: 24px; color: var(--muted); display: flex; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.keys { margin-top: 28px; color: var(--muted); font-size: 13px; }
kbd { border: 1px solid var(--line); border-radius: 4px; padding: 0 5px; font: 12px ui-monospace, monospace; }
.done { padding: 40px 0; text-align: center; }
.name { display: block; margin-top: 6px; font-size: 11px; font-weight: 400; color: var(--muted); overflow-wrap: anywhere; }
</style>
</head>
<body>
<main>
  <h1>Which is more like A: B or C?</h1>
  <p class="sub">The one you'd rather swap in for A in a track. Go with your first impression.</p>
  <div id="rate">
    <div class="row">
      <button class="sound" id="pa">A<small>1</small><span class="name" id="na"></span></button>
      <button class="sound" id="pb">B<small>2</small><span class="name" id="nb"></span></button>
      <button class="sound" id="pc">C<small>3</small><span class="name" id="nc"></span></button>
    </div>
    <div class="row answers">
      <button id="ab">B is closer <small>&larr; / F</small></button>
      <button id="ac">C is closer <small>&rarr; / J</small></button>
      <button class="skip" id="as">Can't tell (S)</button>
    </div>
  </div>
  <div class="done" id="done" hidden>All rated. Run <code>sound_similarity_bench --ratings</code> to fit the weights.</div>
  <div class="status"><span id="progress"></span><span id="pace"></span></div>
  <p class="keys"><kbd>Space</kbd> plays A, B, A, C &nbsp; <kbd>1</kbd><kbd>2</kbd><kbd>3</kbd> play one &nbsp;
    <kbd>U</kbd> undoes the last answer</p>
</main>
<script>
const SECONDS = __SECONDS__;
let ctx = null, current = null, buffers = {}, playing = [], seq = 0, started = Date.now(), answeredHere = 0;
const $ = id => document.getElementById(id);

function audio() { if (!ctx) ctx = new AudioContext(); return ctx; }

async function load(t) {
  const out = {};
  await Promise.all(["a", "b", "c"].map(async slot => {
    const r = await fetch(`/audio/${t.id}/${slot}`);
    if (!r.ok) throw new Error(`${slot}: ${r.status}`);
    out[slot] = await audio().decodeAudioData(await r.arrayBuffer());
  }));
  return out;
}

function stop() {
  seq++;
  for (const p of playing) { try { p.stop(); } catch (e) {} }
  playing = [];
  for (const s of ["a", "b", "c"]) $("p" + s).classList.remove("playing");
}

// Plays one sound, at most SECONDS of it with a short fade; resolves when done.
function play(slot, keep) {
  if (!keep) stop();
  const buffer = buffers[slot];
  if (!buffer) return Promise.resolve();
  const c = audio(), source = c.createBufferSource(), gain = c.createGain();
  source.buffer = buffer;
  const length = Math.min(buffer.duration, SECONDS);
  gain.gain.setValueAtTime(1, c.currentTime + Math.max(0, length - 0.05));
  gain.gain.linearRampToValueAtTime(0, c.currentTime + length);
  source.connect(gain).connect(c.destination);
  source.start(0, 0, length);
  playing.push(source);
  $("p" + slot).classList.add("playing");
  return new Promise(resolve => source.onended = () => { $("p" + slot).classList.remove("playing"); resolve(); });
}

async function playAll() {
  stop();
  const mine = seq;
  for (const slot of ["a", "b", "a", "c"]) {
    if (seq !== mine) return;
    await play(slot, true);
    await new Promise(r => setTimeout(r, 250));
  }
}

async function next() {
  stop();
  const r = await (await fetch("/next")).json();
  $("progress").textContent = `${r.done} of ${r.total} rated`;
  const rate = answeredHere / ((Date.now() - started) / 3600000);
  $("pace").textContent = answeredHere >= 5 ? `${Math.round(rate)} an hour` : "";
  if (!r.triplet) { $("rate").hidden = true; $("done").hidden = false; current = null; return; }
  $("rate").hidden = false; $("done").hidden = true;
  current = r.triplet;
  for (const slot of ["a", "b", "c"]) $("n" + slot).textContent = (current.names || {})[slot] || "";
  try { buffers = await load(current); }
  catch (e) { buffers = {}; $("progress").textContent += ` (couldn't play: ${e.message}; S skips it)`; return; }
  if (ctx && ctx.state === "running") playAll();
}

async function answer(choice) {
  if (!current) return;
  const t = current; current = null;
  await fetch("/answer", { method: "POST", body: JSON.stringify({ id: t.id, choice }) });
  answeredHere++;
  next();
}

async function undo() {
  await fetch("/undo", { method: "POST" });
  answeredHere = Math.max(0, answeredHere - 1);
  next();
}

$("pa").onclick = () => play("a");
$("pb").onclick = () => play("b");
$("pc").onclick = () => play("c");
$("ab").onclick = () => answer("b");
$("ac").onclick = () => answer("c");
$("as").onclick = () => answer("skip");
document.addEventListener("keydown", e => {
  if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
  const k = e.key.toLowerCase();
  if (k === " ") { e.preventDefault(); playAll(); }
  else if (k === "1") play("a");
  else if (k === "2") play("b");
  else if (k === "3") play("c");
  else if (k === "arrowleft" || k === "f") answer("b");
  else if (k === "arrowright" || k === "j") answer("c");
  else if (k === "s" || k === "arrowdown") answer("skip");
  else if (k === "u" || k === "backspace") undo();
});
next();
</script>
</body>
</html>
"""


def read_tsv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line and not line.startswith("#"):
                rows.append(line.split("\t"))
    return rows


class Rater:
    def __init__(self, triplets_path, ratings_path):
        self.triplets = {}
        self.order = []
        for row in read_tsv(triplets_path):
            if len(row) >= 5:
                self.triplets[row[0]] = {"id": row[0], "a": row[2], "b": row[3], "c": row[4]}
                self.order.append(row[0])
        if not self.order:
            sys.exit(f"no triplets in {triplets_path}")
        self.ratings_path = ratings_path
        if not os.path.exists(ratings_path):
            with open(ratings_path, "w", encoding="utf-8") as f:
                f.write("# similarity_rater answers: id, choice (b, c or skip), A, B, C, ms on the question\n")
        self.done = {row[0] for row in read_tsv(ratings_path) if row}
        self.asked_at = time.monotonic()

    def next(self):
        for i in self.order:
            if i not in self.done:
                self.asked_at = time.monotonic()
                return self.triplets[i]
        return None

    def answer(self, triplet_id, choice):
        t = self.triplets.get(triplet_id)
        if t is None or choice not in ("b", "c", "skip") or triplet_id in self.done:
            return
        ms = int(1000 * (time.monotonic() - self.asked_at))
        with open(self.ratings_path, "a", encoding="utf-8") as f:
            f.write("\t".join([t["id"], choice, t["a"], t["b"], t["c"], str(ms)]) + "\n")
            f.flush()
            os.fsync(f.fileno())
        self.done.add(triplet_id)

    def undo(self):
        with open(self.ratings_path, encoding="utf-8") as f:
            lines = f.readlines()
        for i in range(len(lines) - 1, -1, -1):
            if lines[i].strip() and not lines[i].startswith("#"):
                self.done.discard(lines[i].split("\t")[0])
                del lines[i]
                break
        with open(self.ratings_path, "w", encoding="utf-8") as f:
            f.writelines(lines)


def handler(rater, seconds, names):
    page = PAGE.replace("__SECONDS__", repr(float(seconds))).encode("utf-8")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, code, body, kind):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def json(self, value):
            self.send(200, json.dumps(value).encode("utf-8"), "application/json")

        def do_GET(self):
            if self.path == "/":
                self.send(200, page, "text/html; charset=utf-8")
            elif self.path == "/next":
                t = rater.next()
                if t is not None:
                    t = dict(t, names={k: os.path.basename(t[k]) for k in "abc"} if names else None)
                    for k in "abc":
                        del t[k]  # the page plays them by slot; it needn't know where they are
                self.json({"triplet": t, "done": len(rater.done), "total": len(rater.order)})
            elif self.path.startswith("/audio/"):
                parts = self.path.split("/")
                t = rater.triplets.get(parts[2]) if len(parts) == 4 else None
                if t is None or parts[3] not in ("a", "b", "c"):
                    return self.send(404, b"", "text/plain")
                path = t[parts[3]]
                try:
                    with open(path, "rb") as f:
                        body = f.read()
                except OSError:
                    return self.send(404, b"", "text/plain")
                self.send(200, body, mimetypes.guess_type(path)[0] or "application/octet-stream")
            else:
                self.send(404, b"", "text/plain")

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if self.path == "/answer":
                value = json.loads(body or b"{}")
                rater.answer(str(value.get("id")), value.get("choice"))
            elif self.path == "/undo":
                rater.undo()
            else:
                return self.send(404, b"", "text/plain")
            self.json({"ok": True})

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Rate sound triplets by ear (sound_similarity_bench --triplets).")
    parser.add_argument("triplets", help="the triplets, from sound_similarity_bench --triplets")
    parser.add_argument("--ratings", help="where the answers go (default: ratings.tsv next to the triplets)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--seconds", type=float, default=2.5, help="the most of each sound played")
    parser.add_argument("--no-browser", action="store_true", help="don't open the page")
    parser.add_argument("--names", action="store_true", help="show the files' names (not while rating)")
    args = parser.parse_args()
    ratings = args.ratings or os.path.join(os.path.dirname(os.path.abspath(args.triplets)), "ratings.tsv")
    rater = Rater(args.triplets, ratings)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler(rater, args.seconds, args.names))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"{len(rater.done)} of {len(rater.order)} rated; answers go to {ratings}")
    print(f"rating on {url} (Ctrl+C to stop; starting again carries on)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""12 checks: keep-alive behaviour + endpoints + stress, against a RUNNING
farsi-tts-server (default 127.0.0.1:8123)."""
import json, socket, struct, sys, threading, time, urllib.request

HOST, PORT = "127.0.0.1", 8123
if len(sys.argv) > 2:
    HOST, PORT = sys.argv[1], int(sys.argv[2])

results = []
def check(name, ok, info=""):
    results.append(ok)
    print(("  PASS  " if ok else "  FAIL  ") + name + (" — " + str(info) if info else ""))

def recv_one(s, timeout=60):
    """read exactly one HTTP response (head + content-length body)"""
    s.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        b = s.recv(4096)
        if not b:
            return None, buf
        buf += b
    head, _, rest = buf.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1])
    while len(rest) < clen:
        b = s.recv(65536)
        if not b:
            break
        rest += b
    return head, rest[:clen]

def connect(timeout=90):
    return socket.create_connection((HOST, PORT), timeout=timeout)

def req(method, path, body=None, headers=None, timeout=90):
    r = urllib.request.Request(f"http://{HOST}:{PORT}{path}", data=body,
                               method=method, headers=headers or {})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.status, resp.read()

def wav_peak(data):
    """peak amplitude of a PCM16 WAV"""
    i = data.find(b"data")
    if i < 0:
        return 0.0
    n = min(len(data) - i - 4, 200000)
    vals = struct.unpack("<%dh" % (n // 2), data[i + 4:i + 4 + (n // 2) * 2])
    return max(abs(v) for v in vals) / 32768.0

print(f"== final server test @ {HOST}:{PORT} ==")

# 1. keep-alive: many requests on ONE connection, header advertised
ok = True
try:
    s = connect()
    for i in range(8):
        s.sendall(b"GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n")
        head, body = recv_one(s)
        if head is None or b"200" not in head.split(b"\r\n")[0]:
            ok = False
        if b"Keep-Alive:" not in head:
            ok = False
    s.close()
except Exception as e:
    ok = False
    print("     ", e)
check("keep-alive: 8 requests + Keep-Alive header on one connection", ok)

# 2. Connection: close honoured
s = connect()
s.sendall(b"GET /api/health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
head, body = recv_one(s)
eof = s.recv(1) == b""
s.close()
check("Connection: close honoured (header + EOF)",
      b"close" in head.lower() and eof)

# 3. HTTP/1.0 gets no keep-alive (server replies 1.1 but closes the connection)
s = connect()
s.sendall(b"GET /api/health HTTP/1.0\r\nHost: x\r\n\r\n")
head, body = recv_one(s)
eof = s.recv(1) == b""
s.close()
check("HTTP/1.0 default = close",
      b" 200" in head.split(b"\r\n")[0] and b"Connection: close" in head and eof)

# 4. pipelining: two requests in one packet -> two intact responses
s = connect()
s.sendall(b"GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /api/voices HTTP/1.1\r\nHost: x\r\n\r\n")
h1, b1 = recv_one(s)
h2, b2 = recv_one(s)
s.close()
check("pipelined pair answered in order",
      h1 and h2 and b"ok" in b1 and b".wav" in b2)

# 5. bad method -> answered with a 4xx (never a hang/drop)
try:
    code, body = req("DELETE", "/api/health")
    bad_ok = code in (400, 404, 405)
except urllib.error.HTTPError as e:
    code = e.code
    bad_ok = e.code in (400, 404, 405)
check("bad method answered with 4xx", bad_ok, f"code={code}")

# 6. chunked body -> 411, body-less POST -> 400 (both answered, not dropped)
s = connect()
s.sendall(b"POST /api/synth HTTP/1.1\r\nHost: x\r\n"
          b"Transfer-Encoding: chunked\r\n\r\n")
head, body = recv_one(s)
s.close()
s = connect()
s.sendall(b"POST /api/synth HTTP/1.1\r\nHost: x\r\n\r\n")
head2, body2 = recv_one(s)
s.close()
check("chunked -> 411, body-less POST -> 400",
      b" 411" in head and b" 400" in head2,
      head.split(b"\r\n")[0].decode() + " / " + head2.split(b"\r\n")[0].decode())

# 7. oversized body -> 413 answered (not a hang)
s = connect()
s.sendall(b"POST /api/synth HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n")
head, body = recv_one(s, timeout=20)
s.close()
check("oversized body answered 413", b" 413" in head)

# 8. synth over the wire (JSON body), non-silent audio
st, wav = req("POST", "/api/synth?voice=voices/male_hello.wav",
              body=json.dumps({"text": "سلام دنیا"}).encode(),
              headers={"Content-Type": "application/json"}, timeout=180)
peak = wav_peak(wav)
check("POST /api/synth returns non-silent WAV", st == 200 and peak > 0.02,
      f"bytes={len(wav)} peak={peak:.3f}")

# 9. synth + GET on the SAME keep-alive connection
s = connect()
s.sendall(b"POST /api/phonemes HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
          b"Content-Length: %d\r\n\r\n%s" % (len(json.dumps({"text": "سلام"})),
                                             json.dumps({"text": "سلام"}).encode()))
h1, b1 = recv_one(s, timeout=60)
s.sendall(b"GET /api/stats HTTP/1.1\r\nHost: x\r\n\r\n")
h2, b2 = recv_one(s)
s.close()
check("POST phonemes + GET stats on one connection",
      h1 and h2 and b"phonem" in b1.lower() and b"requests" in b2)

# 10. idle keep-alive connection closed by the server (~15 s bound)
s = connect()
s.sendall(b"GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n")
recv_one(s)
t0 = time.time()
s.settimeout(25)
try:
    data = s.recv(1)
    idle_closed = (data == b"")
except socket.timeout:
    idle_closed = False
dt = time.time() - t0
s.close()
check("idle keep-alive closed by server (15 s bound)", idle_closed, f"{dt:.1f}s")

# 11. concurrent synthesis: 4 threads at once, all non-silent
outs, errs = [], []
def worker(i):
    try:
        st, wav = req("POST", "/api/synth?voice=voices/male_hello.wav&seed=%d" % (100 + i),
                      body=json.dumps({"text": "تست هم‌زمانی"}).encode(),
                      headers={"Content-Type": "application/json"}, timeout=300)
        outs.append((st, wav_peak(wav)))
    except Exception as e:
        errs.append(str(e))
ths = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
t0 = time.time()
for t in ths: t.start()
for t in ths: t.join()
check("4 concurrent syntheses all succeed and are audible",
      not errs and len(outs) == 4 and all(st == 200 and p > 0.02 for st, p in outs),
      f"{time.time()-t0:.1f}s errs={errs[:1]}")

# 12. unload beacon (the web UI's pagehide call) releases the model
st, body = req("POST", "/api/unload")
st2, body2 = req("GET", "/api/health")
check("POST /api/unload releases the model",
      st == 200 and b"unloaded" in body and b'"model_loaded":false' in body2,
      body.decode())

n = sum(results)
print(f"\n{n}/{len(results)} checks passed")
sys.exit(0 if n == len(results) else 1)

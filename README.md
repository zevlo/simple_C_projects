# simple_C_projects

## http_server.c

A multithreaded static HTTP server written in C.

### Features

- Serves files from a configurable document root.
- Supports `GET` and `HEAD` with automatic MIME type detection.
- Thread pool with bounded queue for concurrent clients.
- Directory fallback to `index.html`.
- HTML responses for common error codes (400/403/404/405/414/500).
- Structured access logs: `TIMESTAMP IP "METHOD PATH" STATUS BYTES`.

### Usage

```bash
cc -std=c99 -Wall -Wextra -pedantic http_server.c -o http_server -pthread
./http_server /path/to/root 8080 4
```

Arguments (all optional):

1. Document root (default `.`)
2. Port (default `8080`)
3. Worker threads (default `4`, max `8`)

### Load Testing Example

```bash
python3 - <<'PY'
import concurrent.futures as cf
import urllib.request

url = "http://127.0.0.1:8080/index.html"
requests_total = 200
concurrency = 16

def fetch(_):
    with urllib.request.urlopen(url) as resp:
        resp.read(64)

with cf.ThreadPoolExecutor(max_workers=concurrency) as executor:
    list(executor.map(fetch, range(requests_total)))
PY
```

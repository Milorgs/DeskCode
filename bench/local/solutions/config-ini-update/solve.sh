#!/usr/bin/env bash
# Reference solution for config-ini-update (text edits that keep comments).
set -eu
python3 - <<'PY'
import pathlib
p = pathlib.Path("config/app.ini")
s = p.read_text()
s = s.replace("port = 8080\n", "port = 8443\n", 1)
s = s.replace("legacy_mode = true\n", "", 1)
s = s.replace("timeout_seconds = 30\n",
              "timeout_seconds = 30\ntls_cert = /etc/app/tls/cert.pem\ntls_key = /etc/app/tls/key.pem\n", 1)
s = s.replace("pool_size = 5\n", "pool_size = 20\n", 1)
s = s.rstrip("\n") + "\n\n[logging]\nlevel = debug\nfile = /var/log/app/app.log\n"
p.write_text(s)
PY

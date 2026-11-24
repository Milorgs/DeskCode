Update the production configuration `config/app.ini` as follows:

1. In `[server]`: change `port` to `8443`, remove the deprecated `legacy_mode` key, and add
   `tls_cert = /etc/app/tls/cert.pem` and `tls_key = /etc/app/tls/key.pem`.
2. In `[database]`: set `pool_size` to `20`.
3. Add a new section `[logging]` with `level = debug` and `file = /var/log/app/app.log`.

Keep every other section, key, value and all comment lines exactly as they are (the ops
team relies on the comments). Only edit `config/app.ini`; in particular
`config/app.example.ini` must remain unchanged.

# cyberbridge

Reference Python client for the CyberRemesher **local DCC bridge** — the
integration point DCC plugins (the Blender addon and others) build on rather
than speaking raw sockets.

The bridge is local-only and opt-in: the CyberRemesher app binds it to
`127.0.0.1` on explicit user action and shows a listening indicator. No cloud
endpoint exists.

## Install

```sh
pip install ./python        # from the repo root
```

## Usage

```python
from cyberbridge import Client, load_obj

with Client().connect(5140) as app:
    app.push_target(load_obj("highpoly.obj"))   # load geometry as the Target
    app.message("Target loaded — start retopo")
    changed, revision = app.query_changed(0)
    edit = app.pull_editmesh()                    # geometry + UVs the user built
```

Or from the shell:

```sh
python -m cyberbridge --port 5140 ping
python -m cyberbridge --port 5140 push-target model.obj
python -m cyberbridge --port 5140 pull-editmesh out.obj
```

## Protocol

4-byte big-endian length prefix + UTF-8 JSON payload. Every connection opens
with a version handshake (`hello` → `welcome`/`reject`). The full command set
and wire schema are defined in `src/net` (C++ server + reference client); this
package mirrors it exactly.

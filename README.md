# Tether

**English** | [한국어](README.ko.md)

A menu bar app that shares one mouse and keyboard between two Macs.
Push the cursor past a screen edge and control moves to the other Mac; push
back the other way and it returns.

## Requirements

- macOS 13 or later
- Xcode + [xcodegen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- **Accessibility permission** — required on **both** the host and the client
  - Host: capturing keyboard and mouse events (Event Tap)
  - Client: injecting remote clicks and keystrokes (Event Post)
  - System Settings → Privacy & Security → Accessibility → allow Tether

## Build

```bash
bash build.sh          # output: ./bin/Tether.app
bash build.sh clean    # clean build artifacts
bash build.sh cert     # create the tether-dev signing certificate (once)
```

`MARKETING_VERSION` in `project.yml` is the single source of truth for the
version, and it is shown in the menu bar header and the settings window.
Builds never change it — releases set it from the tag (see below).

> Signing with the `tether-dev` certificate keeps accessibility permission
> across rebuilds. Without a certificate the app is signed adhoc, and you have
> to grant permission again after every build.

## Run

```bash
open bin/Tether.app
```

An icon appears in the menu bar and the app runs in the background (no Dock icon).

## Installing on both Macs

Install on the host and the client separately. The architectures can differ
(arm64 / x86_64) — just build on each machine.

```bash
bash build.sh              # run on each Mac
bash make_dmg.sh           # package as dmg (optional)
bash install_app.sh <dmg>  # install into /Applications
```

## Releasing

Versions are managed by tags. Pushing a `v*` tag builds a dmg and publishes it
as a GitHub release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow writes the tag version into `project.yml` before building, so the
app bundle and the dmg filename always match the tag.

## Settings

Click the menu bar icon → **Settings...** (⌘,)

| Item | Description |
|------|-------------|
| Mode | **Host** — sends input / **Client** — receives it |
| Client position | Where the client sits relative to the host (right / left / top / bottom); changeable while running |
| Host IP | The host's IP, used in client mode |
| Port | Defaults to 24900 |
| Shared secret (PSK) | Enables HMAC authentication. Leave empty for no auth. Must **match on both sides** |
| Connection check interval | The client's PING interval (keepalive and reconnect) |
| Launch at login / auto-connect | Start the app and the session automatically after login |

## Usage

1. On the **host**: set mode to `Host` → Start
2. On the **client**: set mode to `Client` → enter the host IP → Start
3. On the host, push the cursor to the configured edge to switch to the client
4. On the client screen, push to the opposite edge to return to the host

### Shortcuts (host)

- **Ctrl+Opt+Cmd+Esc** — force a return to local from remote mode
- **Cmd+Shift+Esc** — force the session to stop (if input is fully stuck)

## What is shared

- Mouse movement, clicks (left / right / middle), and scrolling
- Keyboard, including modifier keys
- Clipboard text and images, in both directions

## Project layout

```
tether/
├── build.sh                 build (auto version bump, signing)
├── test.sh                  tests
├── project.yml              xcodegen config (single source for the version)
└── Sources/
    ├── TetherCore/          C++ core (event capture/injection, UDP, auth)
    └── TetherApp/           Swift UI (menu bar, settings)
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the detailed design.

## License

[MIT](LICENSE) © Myeongjin Cho

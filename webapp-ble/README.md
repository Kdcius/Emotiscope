# Emotiscope BLE remote

The stock Emotiscope remote, adapted to control the device over **Bluetooth LE**
instead of WiFi. Pairs with firmware built with `TRANSPORT_BLE (1)` in
`src/global_defines.h` (see `src/ble_transport.h`).

The device advertises as **"Emotiscope"** using the Nordic UART Service; the app
speaks the exact same pipe-delimited command protocol as the WebSocket version,
newline-framed.

## Requirements

Web Bluetooth support: **Chrome or Edge** on Android, Windows, macOS, or Linux.
iOS Safari/Chrome do not support Web Bluetooth — on an iPhone, open the app in
the free [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055) browser.

Web Bluetooth also requires a **secure context** — `https://` or `localhost`.
Opening `index.html` from the filesystem (`file://`) will not work.

## Hosting

### Quick test on your PC (localhost counts as secure)

```
cd webapp-ble
python -m http.server 8000
```

Then open http://localhost:8000 in Chrome/Edge and click CONNECT.

### On your phone (recommended: GitHub Pages)

A workflow (`.github/workflows/deploy-webapp.yml`) publishes this folder to
GitHub Pages on every push to `main`. One-time setup on your fork:
**Settings → Pages → Source: "GitHub Actions"** (repo must be public, or on a
paid plan). The site lands at `https://<user>.github.io/<repo>/`.

Open that URL in Chrome on Android and **install it to your home screen**
(menu → "Add to Home screen"). A service worker (`sw.js`) caches the whole app,
so after the first load it works fully offline — only Bluetooth is needed in
the field. When you update the app, bump `CACHE_NAME` in `sw.js` so installed
phones fetch the new version.

## Differences from the WiFi remote

- Connection starts from a CONNECT button (browsers only allow the Bluetooth
  device chooser after a user gesture). Reconnection after a drop is automatic.
- Removed: firmware update check (OTA is WiFi-only), WiFi config mode, cloud
  discovery, MAC display.
- Single client at a time (the firmware stops advertising while connected).

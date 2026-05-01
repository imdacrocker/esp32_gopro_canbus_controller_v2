# Web UI Specification — GoPro CAN-BUS Controller

> **Baseline:** Current `index.html` (single-file, 1385 lines, ~54 KB).
> V2 will serve pre-gzipped files from LittleFS.  This document captures
> every detail of the present implementation so V2 can be rebuilt from scratch
> without consulting the original file.
>
> **CHANGE MARKERS:** Sections or items marked `<!-- TODO -->` are places where
> changes are expected or a decision is still open.

---

## 1. Delivery & Build

| Property | Current | V2 Target |
|---|---|---|
| File structure | Single `index.html` (inline CSS + JS) | Separate `index.html`, `app.css`, `app.js` pre-gzipped at build time |
| Served from | Embedded in firmware via `EMBED_TXTFILES` | LittleFS data partition (3 MB `storage` partition already in `partitions.csv`) |
| iOS caching fix | Not yet applied | Serve with explicit `Content-Length` header |
| Encoding | UTF-8 | UTF-8 |

The single-file format remains acceptable for V2 if the gzip step is handled by the build system; splitting is optional.

---

## 2. Layout & Viewport

```
max-width: 480px
margin: 0 auto
padding: 0 1em 5em   ← 5em bottom clearance for fixed bar
font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif
font-size (root): 20px   ← all rem values multiply from this
```

The page is intentionally narrow/mobile-first. It is usable on a phone held in one hand at the side of a race car.

### Vertical stacking order (top to bottom, in DOM order)

1. Sticky page header (gear icon)
2. RaceCapture Status section
3. Auto-Control section
4. `<p id="status">` — transient feedback line
5. Camera Status section (control bar + camera cards)
6. Fixed bottom bar ("Add / Manage Cameras" button)

---

## 3. Color Palette & Design Tokens

All values used verbatim in the current CSS:

```css
/* Blues — connected, buttons, links */
--blue:        #2980b9;
--blue-hover:  #2471a3;

/* Greens — recording, start button, auto-control ON, logging */
--green:       #27ae60;
--green-dark:  #2e7d32;   /* recording badge text only */
--green-hover: #219a52;   /* legacy add button hover */

/* Oranges */
--orange:      #f39c12;   /* not-recording badge, RC not-logging */
--orange-dark: #e67e22;   /* reboot button */
--orange-dark-hover: #ca6f1e;

/* Reds — stop, forget, reset, danger */
--red:         #e74c3c;
--red-hover:   #c0392b;
--red-dark-hover: same as red-hover

/* Grays */
--gray-text:   #888;      /* section titles, settings-btn icon */
--gray-light:  #999;      /* secondary labels, disconnected badge */
--gray-label:  #555;      /* UTC time, status feedback text */
--gray-bg:     #f8f8f8;   /* section title backgrounds */
--gray-border: #ddd;      /* most borders */
--gray-border-light: #eee; /* card separators */
--gray-disabled: #e0e0e0; /* disabled shutter button background */
--gray-disabled-text: #bbb;
--gray-toggle-off: #ccc;  /* toggle track when off */

/* Text */
--text-primary:   #222;
--text-secondary: #555;
--white: #fff;

/* Backgrounds */
--card-bg:  #fafafa;
--modal-bg: #fff;
--page-bg:  #fff;
--overlay:  rgba(0, 0, 0, 0.45);
```

<!-- CSS custom properties (`:root { --blue: … }`) should be adopted in V2 for maintainability. -->

---

## 4. Typography Scale

| Class / Element | font-size | font-weight | Notes |
|---|---|---|---|
| `h1` in header | `1.15rem` (23px) | 600 | letter-spacing 0.02em |
| Section titles (`.rc-section-title`, etc.) | `0.7rem` (14px) | 700 | UPPERCASE, letter-spacing 0.1em, color `#888` |
| `cam-number` | `0.95rem` | 600 | |
| `cam-display-name` | `0.7rem` | 700 | UPPERCASE, letter-spacing 0.06em, color `#999` |
| `cam-model-name` | `0.72rem` | 400 | color `#999` |
| Status badge | `0.88rem` | 600 | |
| RC value | `1.05rem` | 600 | |
| RC label | `0.68rem` | 400 | UPPERCASE, letter-spacing 0.08em, color `#999` |
| UTC date line | `0.72rem` | 600 | monospace, color `#888` |
| UTC time line | `0.88rem` | 600 | monospace, color `#555` |
| Auto-control label | `0.92rem` | 600 | color `#222` |
| Auto-control sub | `0.72rem` | 400 | color `#888`, line-height 1.4 |
| Toggle state text | `0.78rem` | 600 | min-width 26px |
| Buttons (primary) | `0.88rem` | 700 | |
| Buttons (control bar) | `0.85rem` | 700 | |
| Settings row label | `0.9rem` | 600 | |
| Settings select | `0.85rem` | 400 | |
| Modal title | `1rem` | 700 | |
| Modal section title | `0.65rem` | 700 | UPPERCASE, letter-spacing 0.1em, color `#999` |
| Paired cam name | `0.9rem` | 600 | |
| Paired cam meta | `0.72rem` | 400 | color `#999` |
| Found cam name | `0.88rem` | 600 | |
| Found cam addr | `0.72rem` | 400 | color `#999` |
| Type badge | `0.6rem` | 700 | UPPERCASE, letter-spacing 0.06em |
| Empty message | `0.88rem` – `0.9rem` | 400 | color `#888` |

---

## 5. Animations

```css
/* Pulsing dot on the recording status badge */
@keyframes cam-pulse {
    0%, 100% { opacity: 1; }
    50%       { opacity: 0.25; }
}
animation: cam-pulse 1.2s infinite;

/* Loading spinner (shown before first camera poll) */
@keyframes spin {
    to { transform: rotate(360deg); }
}
animation: spin 0.75s linear infinite;
/* Spinner: 28×28px, 3px border, border-color #e0e0e0, border-top-color #2980b9 */
```

---

## 6. Page Header

```
id: page-header
position: sticky, top: 0, z-index: 50
background: #fff
border-bottom: 1px solid #ddd
padding: 16px 20px
```

- **Title:** "GoPro CAN-BUS Controller" — centered, `h1`, `1.15rem`, weight 600
- **Settings button** (`id="settings-btn"`): absolute, vertically centered, right edge (right: 16px)
  - Gear SVG icon, 22×22, stroke currentColor
  - Default color: `#888`; hover: `#333`, background `#f0f0f0`
  - Opens the Settings top-sheet modal

<!-- Title is "GoPro CAN-BUS Controller" — update if desired. -->

---

## 7. RaceCapture Status Section

```
id: rc-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0.4em 0 0.8em
```

**Section title bar:** "RaceCapture Status", uppercase, `0.7rem`, weight 700, background `#f8f8f8`, border-bottom `#ddd`

**Two-cell grid** (`display: grid; grid-template-columns: 1fr 1fr`):

| Cell | Label | Value element | States |
|---|---|---|---|
| Left | "Logging" | `#rc-logging-pill` | See below |
| Right | "RC Date/Time (Local)" | `#utc-display` | See below |

**Logging pill classes** (applied to `#rc-logging-pill`):
```
rc-value rc-logging      → color: #27ae60   text: "Logging"
rc-value rc-not-logging  → color: #f39c12   text: "Not Logging"
rc-value rc-unknown      → color: #e74c3c   text: "Unknown"
```
CSS rule: `el.className = 'rc-value rc-' + state.replace('_', '-')`

**UTC / Local time display:**
- Two `<div>` children inside `#utc-display` (font-family: monospace)
- `#utc-date-line`: `YYYY-MM-DD`, `0.72rem`, color `#888`
- `#utc-time-line`: `HH:MM:SS`, `0.88rem`, color `#555`
- When GPS not yet valid: date line = "No GPS", time line = ""
- Firmware returns epoch_ms with timezone offset already applied; JS reads getUTC* methods

---

## 8. Auto-Control Section

```
id: auto-control-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0 0 0.8em
```

Single row (`#auto-control-row`, padding 14px, flex, space-between):

**Left side (text):**
- Label: "Automatic Control", `0.92rem`, weight 600, color `#222`
- Sub-label: "Cameras will start and stop recording automatically based on the RaceCapture logging status.", `0.72rem`, color `#888`

**Right side (toggle):**
- State label: "On" (green `#27ae60`) / "Off" (gray `#999`), `0.78rem`, weight 600
- Custom toggle: 56×32px track, 24px thumb
  - Track ON: background `#27ae60`; OFF: background `#ccc`
  - Thumb: always white, slides left→right (left: 4px → 28px)
  - Transition: `background 0.2s`, `left 0.2s`
- Clicking the entire `.toggle-wrap` calls `setAutoControl(!autoControlEnabled)`

**Effect on Camera Status section:**
- Auto ON → control bar (`#control-bar`) hidden; per-camera shutter buttons hidden
- Auto OFF → control bar shown (2-col grid); per-camera shutter buttons shown on connected cameras

---

## 9. Status Feedback Line

```html
<p id="status"></p>
```
- Color `#555`, `0.5em 0 1em` margin, min-height `1.2em`
- Transient text set by `setStatus(msg)` after shutter commands
- Cleared automatically only by subsequent commands (not on a timer)

---

## 10. Camera Status Section

```
id: cam-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0 0 0.8em
```

**Section title bar:** "Camera Status", same style as RaceCapture title bar

**Control bar** (`#control-bar`):
- Shown only when Auto-Control is OFF
- 2-column grid, gap 10px, padding 14px, border-bottom `#ddd`
- "Record All" button: green `#27ae60`, calls `sendShutter(true)`
- "Stop All" button: red `#e74c3c`, calls `sendShutter(false)`
- Both buttons: `0.85rem`, weight 700, min-height 48px, border-radius 8px
- Both disabled during in-flight request; re-enabled in `.finally()`

**Camera list** (`#cam-status-list`):

Initial state: shows spinner (`#cam-status-loading`) until first poll resolves.

Empty state: `<span id="cam-status-empty">No cameras paired.</span>`, color `#888`, `0.9rem`

**Camera card** (`.camera-card`):
```
padding: 16px 14px
border-bottom: 1px solid #eee
```

Card internal layout:
```
.cam-meta  (flex, align-items: baseline, gap: 8px, margin-bottom: 4px)
  .cam-number       "Cam {index}"    0.95rem, weight 600, #222
  .cam-display-name  {cam.name}       0.7rem,  weight 700, UPPERCASE, #999
  .cam-type-badge   "WiFi RC"        (RC-emulation cameras only — see §10.2)

.cam-model-name  {cam.model_name}  0.72rem, #999, margin-bottom 12px
  (omitted when model_name is absent or equals cam.name)

.cam-footer  (flex, align-items: center, justify-content: space-between, gap: 10px)
  .status-badge  + optional shutter button
```

### 10.1 Status Badge

```
.status-badge  display: inline-flex, align-items: center, gap: 8px
               font-weight: 600, font-size: 0.88rem
.status-dot    border-radius: 50%, flex-shrink: 0
```

| API `status` | CSS class | Dot | Text color | Label |
|---|---|---|---|---|
| `disconnected` | `disconnected` | hidden (`display: none`) | `#999` | "Not Connected" |
| `connected` | `connected` | 9×9px, `#2980b9` (solid) | `#2980b9` | "Connected" |
| `not_recording` | `not-recording` | 9×9px, `#f39c12` (solid) | `#f39c12` | "Not Recording" |
| `recording` | `recording` | 9×9px, `#27ae60`, `cam-pulse 1.2s` | `#2e7d32` | "Recording" |

### 10.2 Type Badge (RC-emulation cameras)

```
.cam-type-badge
  font-size: 0.6rem, font-weight: 700, UPPERCASE, letter-spacing 0.06em
  padding: 2px 6px, border-radius: 4px
  background: #e8f4fd, color: #2980b9, border: 1px solid #aad4f0
```
Shown on camera cards and paired rows when `cam.type === 'rc_emulation'`. Badge text: "WiFi RC".

### 10.3 Per-Camera Shutter Buttons

Rendered only when **Auto-Control is OFF** and camera is `not_recording` or `recording`:

```
.cam-shutter-btn  border-radius: 8px, font-size: 0.88rem, font-weight: 700
                  padding: 11px 20px, min-height: 44px
.cam-shutter-start  background: #27ae60 (green)   text: "Record"
.cam-shutter-stop   background: #e74c3c (red)     text: "Stop"
disabled state:     background: #e0e0e0, color: #bbb
```

**Shutter lock (5s):** After tapping a per-camera button:
1. Button disabled immediately
2. `shutterLocked[slot] = { expectedStatus, timer }` — 5s timeout
3. Next `refreshCameraStatus()` checks if camera reached expected state; if so, clears lock early
4. Lock auto-expires after 5s regardless

Calls `POST /api/shutter` with `{ slot, on }`.

---

## 11. Fixed Bottom Bar

```
id: manage-bar
position: fixed, bottom: 0, left/right: 0
background: #fff, border-top: 1px solid #ddd
padding: 0.75em 1em
z-index: 100
```

Single button `#manage-btn`: "+ Add / Manage Cameras", blue `#2980b9`, full-width (max 480px), 48px min-height. Opens Manage Cameras bottom-sheet modal.

---

## 12. Settings Modal (Top Sheet)

Triggered by the gear icon in the page header.

```
id: settings-overlay
position: fixed, inset: 0
background: rgba(0,0,0,0.45)
z-index: 200
aligns: flex-start (sheet anchors to top)
```

**Sheet panel** (`.settings-modal`):
```
background: #fff
border-radius: 0 0 12px 12px   ← only bottom corners rounded
width: 100%, max-width: 480px
padding: 0 0 1.5em
```

**Header:** "Settings" title left, "Done" button right (blue, closes modal)

Clicking the overlay backdrop (not the modal card) also closes the modal.

**Section 1 — Device:**
- Title: "DEVICE", `0.65rem`, UPPERCASE, color `#999`
- Single row: label "Time Zone" left, `<select>` right
  - Select options: UTC-12 … UTC+14 (whole hours), populated by `buildTimezoneDropdown()` on page load
  - Labels: "UTC-12" … "UTC" … "UTC+14"
  - On change: `POST /api/settings/timezone` with `{ tz_offset_hours: int }`
  - On open: `GET /api/settings/timezone` → sets selected value
- **Set Date & Time row** — only rendered when `gps_valid == false` (checked at modal open via `GET /api/utc`):
  - Label "Set Date & Time" left, `<button>` "Set from Device" right
  - Button style: blue outline, `0.85rem`, `min-height: 36px`, `border-radius: 6px`
  - On tap: reads `Date.now()` from the browser, `POST /api/settings/datetime` with `{ epoch_ms: number }`
  - On success: brief inline confirmation "Time set ✓" replaces button for 2 s, then restores
  - On error: inline "Failed — try again" in red for 2 s
  - Row is hidden (not just disabled) when GPS is providing valid UTC, so it does not clutter the UI during normal race operation

**Reboot button:**
```
display: block, width: calc(100% - 32px), margin: 16px 16px 0
background: #e67e22 (orange), color: #fff
font-size: 0.88rem, font-weight: 700, min-height: 48px, border-radius: 8px
```
- Confirm dialog: "Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved."
- On confirm: `POST /api/reboot`, disable button, show "Rebooting…" with dot animation
- After 5s: `location.reload()`

**Factory Reset button:**
```
background: #e74c3c (red)
```
- Confirm dialog: "Restore Defaults?\n\nThis will erase all paired cameras and settings, then restart the controller. This cannot be undone."
- On confirm: `POST /api/factory-reset`, same animation pattern → `location.reload()` after 5s

---

## 13. Manage Cameras Modal (Bottom Sheet)

Triggered by "Add / Manage Cameras" button.

```
id: modal-overlay
position: fixed, inset: 0
background: rgba(0,0,0,0.45)
z-index: 200
aligns: flex-end (sheet anchors to bottom)
```

**Sheet panel** (`.modal`):
```
background: #fff
border-radius: 12px 12px 0 0   ← only top corners rounded
width: 100%, max-width: 480px
max-height: 85vh, overflow-y: auto
padding: 0 0 2em
```

**Header:** "Manage Cameras" title left, "Done" button right. Sticky (z-index 10) so it stays visible while scrolling.

On open: immediately calls `refreshModalPairedCameras()` and `refreshLegacyDiscovered()`, then starts a 3s interval for both.

On close: cancels any active BLE scan, clears the 3s refresh interval, resets all modal UI state.

Clicking backdrop also closes modal.

### 13.1 Section 1 — Pair a New Camera (BLE)

Title: "PAIR A NEW CAMERA"

**Scan button** (`#scan-btn`):
- Idle: blue `#2980b9`, text "Scan for Cameras"
- Scanning: background `#f0f0f0`, border `1px solid #ccc`, color `#333`, text "Cancel Scan"
- Toggle behavior: if scanning → cancel; else → start

**Scan flow:**
1. `POST /api/scan` → start 1s poll on `GET /api/cameras` + `GET /api/paired-cameras`
2. Show countdown: "Scanning… 120s" decrementing every second
3. Filter discovered list to exclude already-paired addresses
4. Render unpaired cameras in `#results` as `.found-camera-row` cards:
   - Name (`0.88rem`, weight 600) + address + RSSI (`0.72rem`, `#999`)
   - "Pair" button (blue, `pair-this-btn`)
5. Auto-stop at 120s (121s timer to account for last second display); final poll on stop
6. Status line (`#modal-status`): shows countdown / "Scan complete." / "Scan cancelled." / errors

**Pairing:**
- Click "Pair" → cancel active scan → `POST /api/pair` with `{ addr, addr_type }`
- Status: "Pairing initiated — camera should appear in the list shortly."
- Result list cleared; new camera will appear on next camera status poll

### 13.2 Section 2 — Add WiFi RC Emulation Camera

Title: "ADD WIFI RC EMULATION CAMERA"

**Refresh List button** (`#rc-add-btn`): green `#27ae60`

On open and on button click: `GET /api/rc/discovered`
- Returns array of `{ addr, ip }` for SoftAP-connected stations that haven't been identified yet
- 0 results: "No unidentified devices connected."
- N results: "{N} device(s) connected — click Add to probe:"

Each unidentified device renders as a `.found-camera-row`:
- Name: "Unknown Device"
- Meta: `{addr} — {ip}` (or "IP pending" if no IP)
- "Add" button (blue, `pair-this-btn`)

**Add flow:**
1. If IP missing: show "Cannot add — IP address not yet assigned. Wait a moment and click Refresh List." and abort
2. `POST /api/rc/add` with `{ addr, ip }` — firmware defaults to `HERO4_BLACK`; no model picker in UI
3. Show "Probing device {addr}… (up to 15 s)", disable UI
4. After 15s: `GET /api/rc/discovered`
   - If addr gone from list → "✅ Camera added — it will appear in the camera list shortly."
   - If addr still in list → "⚠️ Could not identify {addr} as a GoPro camera."
5. Re-enable button, refresh all lists

### 13.3 Section 3 — Paired Cameras

Title: "PAIRED CAMERAS" + count badge (hidden when 0)

Source: `GET /api/paired-cameras`

Each camera renders as a `.modal-paired-row` (background `#fafafa`, border `#eee`, border-radius 8px):
- Left: name line (`0.9rem`, weight 600) with optional type badge (shown when `cam.type === 'rc_emulation'`, text "WiFi RC"); meta line (`0.72rem`, `#999`) showing model_name · Cam {index} [· addr if RC-emulation]
- Right: "Forget" (BLE) or "Remove" (RC-emulation) button in red `#e74c3c`

**Remove flow:** Confirm dialog → `POST /api/remove-camera` with `{ slot }` → refresh lists. RC-emulation removal also waits 1.5s then refreshes `GET /api/rc/discovered` (async slot free).

---

## 14. Polling Summary

| Timer | Interval | Endpoints | Purpose |
|---|---|---|---|
| Camera status | 3s | `GET /api/paired-cameras` | Camera cards on main screen |
| RC status + UTC + auto-control | 2s | `GET /api/logging-state`, `GET /api/utc`, `GET /api/auto-control` | Top two sections |
| BLE scan results | 1s (during scan only) | `GET /api/cameras`, `GET /api/paired-cameras` | Found cameras list in modal |
| Modal refresh | 3s (modal open only) | `GET /api/paired-cameras`, `GET /api/rc/discovered` | Paired list + RC-emulation list in modal |

All polls fire independently via `setInterval`; no coordination or debouncing between timers.

---

## 15. HTTP API Contract (consumed by this UI)

| Method | Path | Request body | Response body | Notes |
|---|---|---|---|---|
| GET | `/api/logging-state` | — | `{ state: "logging"\|"not_logging"\|"unknown" }` | |
| GET | `/api/utc` | — | `{ valid: bool, epoch_ms: int }` | epoch_ms has tz offset applied |
| GET | `/api/auto-control` | — | `{ enabled: bool }` | |
| POST | `/api/auto-control` | `{ enabled: bool }` | `{ enabled: bool }` | |
| GET | `/api/cameras` | — | `[{ name, addr, addr_type, rssi }]` | BLE scan results |
| GET | `/api/paired-cameras` | — | `[{ slot, index, name, model_name, type, addr, status }]` | `type`: `"ble"` or `"rc_emulation"`; `status`: `"disconnected"\|"connected"\|"not_recording"\|"recording"` |
| POST | `/api/scan` | — | `{}` | Starts BLE scan |
| POST | `/api/scan-cancel` | — | `{}` | Cancels BLE scan |
| POST | `/api/pair` | `{ addr, addr_type }` | `{}` | Initiates BLE pairing |
| POST | `/api/remove-camera` | `{ slot }` | `{}` | Removes paired camera (both types) |
| POST | `/api/shutter` | `{ on: bool }` or `{ slot, on: bool }` | `{ dispatched: int }` | Omit `slot` for all cameras |
| GET | `/api/rc/discovered` | — | `[{ addr, ip }]` | Unprobed SoftAP stations |
| POST | `/api/rc/add` | `{ addr, ip }` | `{}` | Starts async probe; firmware defaults to `HERO4_BLACK` |
| POST | `/api/reboot` | — | `{}` or no response | ESP32 may drop connection before responding |
| POST | `/api/factory-reset` | — | `{}` or no response | Same as above |
| GET | `/api/settings/timezone` | — | `{ tz_offset_hours: int }` | |
| POST | `/api/settings/timezone` | `{ tz_offset_hours: int }` | `{}` | |
| POST | `/api/settings/datetime` | `{ epoch_ms: number }` | `{}` | Only valid when `gps_valid == false`; sets system time from browser clock; triggers `open_gopro_ble_sync_time_all()` |

---

## 16. JavaScript State Variables

| Variable | Type | Initial value | Purpose |
|---|---|---|---|
| `autoControlEnabled` | bool | `true` | Mirrors firmware auto-control flag |
| `shutterLocked` | object | `{}` | Per-slot lockout: `{ expectedStatus, timer }` |
| `scanning` | bool | `false` | BLE scan in progress |
| `cameraStatusLoaded` | bool | `false` | Suppresses spinner after first poll |
| `pollTimer` | interval ID | `null` | 1s BLE scan poll |
| `countdownTimer` | interval ID | `null` | 1s scan countdown display |
| `modalPairedRefreshTimer` | interval ID | `null` | 3s modal refresh |

---

## 17. Open Items / Known V2 Decisions

- **Camera type field:** V2 API uses `"rc_emulation"` (not `"legacy_wifi"`). UI badge text is "WiFi RC". ✅ Resolved.
- **Model selection for RC-emulation cameras:** Firmware defaults to `HERO4_BLACK`; no model picker in the UI. ✅ Resolved (deferred).
- **COHN cameras:** No UI for provisioning COHN credentials. V2 may trigger the flow from BLE pairing completion rather than a separate UI step — TBD.
- **COHN re-provisioning:** No trigger in the UI if credentials become stale. User would need a way to re-initiate BLE provisioning from a paired camera row — TBD.
- **BLE status granularity:** Four states (`disconnected` / `connected` / `not_recording` / `recording`) match V2 camera_manager — no UI change needed.
- **Color palette:** V2 should use CSS custom properties (`:root { --blue: #2980b9; … }`) for maintainability.
- **Settings → Device section:** May need additional entries (e.g. per-camera name edit) — TBD.
- **Timezone half-hours:** Whole-hour offsets only. Not a V2 priority (e.g. UTC+5:30 would need `float` or half-hour `int`).

# S63 charts on the MFD — design

## Purpose

S63 is the IHO standard for encrypted electronic navigational charts. Many official chart authorities distribute their ENCs as S63: o-charts (paid, our partner), LINZ in New Zealand (free), UKHO Primar (paid), and others. The plugin should make installing and managing S63 charts on the MFD feel as simple as installing an app, regardless of which authority issued them — and it must never ask the user to type cryptographic strings, copy permit files into the right folder, or interpret SSE error codes.

This document covers two repos:

- `s63_pi` — the OpenCPN plugin (cryptography, chart rendering, `DpS63API` headless layer)
- `deeprey-gui` — the MFD UI, specifically the `Charts → S63 charts` sub-panel

## What the user sees

Two states of the S63 sub-panel. All other surfaces (chart options, basemap, o-charts oeSENC) are unchanged.

### State 1 — Empty (no S63 charts installed)

The layout walks the user through the three-step round-trip: save the device's activation file to a USB, take it to a phone or laptop to register with a chart authority, then bring the chart files back and import them.

```
┌──────────────────────────────────────────────────────────┐
│  S63 encrypted charts                                    │
│  Official ENCs with cryptographic protection.            │
│                                                          │
│  ─── 1. Save your activation file ───                    │
│                                                          │
│  Insert a USB stick, then tap:                           │
│  [ Save activation file to USB ]                         │
│                                                          │
│  Device ID: A1B2-C3D4-E5F6-7890                          │
│                                                          │
│  ─── 2. Register and buy charts ───                      │
│                                                          │
│  Take the USB to a phone or laptop, register with any    │
│  S63 chart authority, and upload the activation file     │
│  when prompted. Save the chart files they send back      │
│  onto the same USB stick.                                │
│                                                          │
│  ─── 3. Import your charts ───                           │
│                                                          │
│  Bring the USB back, insert it, then tap:                │
│  [ Import from USB ]                                     │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### State 2 — Charts installed

```
┌──────────────────────────────────────────────────────────┐
│  S63 charts                              [+ Add charts]  │
│  12 installed · 2 expiring soon                          │
│  Device ID: A1B2-C3D4-E5F6-7890  [Save activation file]  │
│                                                          │
│  GB502106                                                │
│  o-charts · Expires in 12 days                      ⚠    │
│                                                          │
│  NZ500001                                                │
│  LINZ · Valid until 2027-03-15                      ✓    │
│                                                          │
│  FR201478                                                │
│  o-charts · Expired                                 ✗    │
└──────────────────────────────────────────────────────────┘
```

**Per-row content** (all fields populated from `DpS63CellInfo` today):

- Line 1: cell ID (`cellName`)
- Line 2: vendor badge (`producer` / Data Server ID) · status text (computed from `status` + `expiryDate`)
- Trailing pill: ⚠ Expiring soon (≤30 days, via `DpS63Strings::ExpiringSoon`), ✓ Valid, or ✗ Expired

**Sort order**: Expiring → Valid → Expired. Rows within a group sorted by cell name.

**Header**:

- Counter "{N} installed · {M} expiring soon" computed by `DpS63ChartsPanel` from the cell list.
- Device-ID row exposes the same handle and `Save activation file` action shown in the empty state — so registering with a second vendor or attaching the file to a support ticket needs no extra navigation.

**Per-row action**: tap row → confirmation dialog *"Remove {cellName}?"* with Remove / Cancel. Remove calls `DpS63API::RemoveCell`. No detail sheet in MVP.

**`+ Add charts`**: triggers the USB import flow. Re-importing a `PERMIT.TXT` containing an updated permit for an installed cell replaces the existing `.os63` — this is the renewal and update path.

Certificate management is handled automatically during import. Import diagnostics are written to `/home/opencpn/.opencpn/opencpn.log`. `userpermit` and `installpermit` are device-internal and never surface in the UI.

## Auto-generated device identity

The cryptographic identity is built around a single artefact: the **activation file** (`.fpr`), generated on the device on first boot. The user never types, scans, or copies any cryptographic string.

On the first call to `s63_pi::Init()`:

1. Invoke `OCPNsenc -w -o <identity-dir>` to write the activation file.
2. Compute the **device ID** = SHA-1 of the activation file's contents, first 16 hex chars, formatted `XXXX-XXXX-XXXX-XXXX`. This is the human-readable handle shown in the UI.
3. Write a `provisioned.json` sentinel next to the activation file so subsequent boots skip regeneration.

The identity store is a fixed directory under the plugin's config path, owned by `s63_pi`. The activation file is hardware-bound; it survives plugin upgrades and OS updates as long as the device's hardware identifier is unchanged.

Per the S63 spec, the `userpermit` and `installpermit` are produced by the chart authority — not on the device — using the authority's manufacturer key applied to the activation file. They arrive back inside the same USB bundle that contains the chart files, and the import pipeline picks them up automatically. They are persisted in the identity store and never surface in the UI.

`DpS63Panel::EvaluateMode()` routes between `Empty` and `Charts` based on whether any `.os63` files exist; the activation file is always provisioned. `DpS63EmptyPanel` renders the **Empty state** described above.

## The activation file — the only crypto the user ever handles

The activation file is the single artefact that flows between the device and any S63 chart authority. The user never types, scans, or pastes cryptographic strings; they only move a file.

`Save activation file to USB` copies two things onto the stick:

- The `.fpr` itself — the file the vendor's website ingests.
- A small `DEVICE_ID.txt` containing the human-readable device ID — for support tickets and for distinguishing two MFDs on the same boat.

After registering on the vendor's site and uploading the `.fpr`, the user receives back a chart bundle (typically `PERMIT.TXT` + an `ENC_ROOT/` directory, possibly with sibling `USERPERMIT.TXT` / `INSTALLPERMIT.TXT` files). They drop the bundle onto the same USB stick and tap `Import from USB`. The import pipeline reads `SERIAL.ENC` + `CATALOG.031` to recognise any compliant S63 bundle regardless of issuing authority.

## Import pipeline

USB import is **button-triggered**, matching the pattern used elsewhere in `deeprey-gui` (Chart Manager, Routes, Layers, Tracks, Bathymetry, Screenshots).

1. User taps `Import from USB`. The handler calls `DetectUSBMountPoint()` (in `src/utils/DpGUIDialogs.cpp`), which shells out to `/usr/bin/deeprey-detect-usb` (system tool in `deeprey-system-config`). That tool finds a removable drive and mounts it at the fixed path `/media/deeprey-usb` (tries vfat → exfat → ext4).
2. If no drive is found, show a toast: *"Insert a USB stick first."* — `DpToast`, non-modal, matching the existing S63 panel pattern.
3. The `DpS63API` layer scans the mount for `PERMIT.TXT`, `SERIAL.ENC`, `CATALOG.031`, `ENC_ROOT/`, loose `.PUB` files, and any sibling text file carrying the userpermit/installpermit pair (named `USERPERMIT.TXT` / `INSTALLPERMIT.TXT` / `KEYS.TXT`, or embedded as header lines in `PERMIT.TXT`).
4. If the bundle carries a userpermit + installpermit pair, both are persisted to the identity store before permit validation runs. The pair is per-vendor; re-importing from a different authority later writes a fresh pair without disturbing previously installed cells.
5. New `.PUB` certificates are imported automatically.
6. Permits are validated via `OCPNsenc -d -p <permit> -u <userpermit> -e <installpermit>` and stored as `.os63` metadata files.
7. Each encrypted cell is authenticated against the matching publisher certificate and decrypted with `OCPNsenc -n -i <cell> -o <senc> -u <userpermit> -e <installpermit> -p <cellpermit> -z <pluginpath>`, producing an eSENC.
8. The user sees a progress indicator during the import and a plain-language summary at the end. SSE codes (6, 8, 9, 15, 24, 26, …) are translated into sentences for the user; the raw codes go to `/home/opencpn/.opencpn/opencpn.log` for support to read.

eSENC files are always precomputed during import; no prompt.

### Button label and progress UX alignment

The button label is `Import from USB` everywhere it appears — empty state, charts state's `+ Add charts` action, `DpS63SetupPanel`, and `DpS63ChartsPanel` — matching Chart Manager.

Progress feedback uses `wxProgressDialog` with stage messages, matching Chart Manager's import UX.

## Component changes

| Component | Repo | Description |
| --- | --- | --- |
| `s63_pi::Init()` | `s63_pi` | Generate the activation file on first run, compute the device ID, write the `provisioned.json` sentinel. Subsequent boots are a no-op. |
| `DpS63API` | `s63_pi` (deeprey-api) | Add `GetDeviceIdString()` (the human-readable handle), `ExportActivationFileToUsb(path)` (copy `.fpr` + `DEVICE_ID.txt` onto a USB mount), and `ImportFromUsb(path, progress, complete)` (single-call orchestration of cert + permit + cell import, including auto-pickup of any userpermit/installpermit pair shipped in the bundle). `GetInstalledCells()` and `RemoveCell()` already return / accept everything else the panel needs. |
| `DpS63Panel::EvaluateMode()` | `deeprey-gui` | Two modes only: `Empty` (no `.os63` files exist) and `Charts` (at least one exists). |
| `DpS63EmptyPanel` | `deeprey-gui` | Renders the three-step Empty state: save the activation file, register and buy charts, import the bundle. No permit-entry inputs anywhere. |
| `DpS63ChartsPanel` | `deeprey-gui` | Header summary (`N installed · M expiring soon`) + device-ID row (handle + `Save activation file` button) + `+ Add charts` button. Sort cells: Expiring → Valid → Expired, then by name. Tap row → confirm-and-remove dialog. |
| `DpS63CellCard` | `deeprey-gui` | Render as: cell ID, `{producer} · {status text}` line, trailing pill (✓ Valid / ⚠ Expiring soon / ✗ Expired). Tap surface to fire the row-remove signal. |
| `DpS63AdvancedPanel` | `deeprey-gui` | Not surfaced in the sub-panel navigation. Existing class compiled but unused; available for future re-use. |

## Out of scope for MVP

- In-app o-charts login / direct API pull of permits and cells
- Wireless transfer from phone to MFD (local web upload, cloud relay)
- Auto-renewal nudges
- Location-aware vendor suggestions
- A vendor catalog of any kind in the chart settings UI

## Testing

- First-boot identity generation writes the activation file and `provisioned.json` sentinel; the activation file matches what `OCPNsenc -w -o` would write today.
- Empty state appears when zero `.os63` files exist; Charts state appears otherwise.
- `Save activation file to USB` produces a stick containing the `.fpr` file plus `DEVICE_ID.txt`; the `.fpr` uploads cleanly to at least one chart authority's web form.
- A free LINZ NZ bundle and an o-charts bundle import through the same flow and produce rows with `LINZ` and `o-charts` badges respectively.
- Expired permits surface as `Expired` pills with a clear non-cryptographic message.
- The `Save activation file` action is reachable from the Charts-state header row and from the empty state — never required to dig deeper.

# Direct USB transport for the sender — design

Date: 2026-10-17
Branch: `feat/direct-usb-transport`
Target: upstream PR to `swellweb/targetBridge` (keep upstream style: 5-language L10n, XCTest coverage)

## Problem

When two Macs are linked by a USB-C / USB4 cable, macOS exposes a USB-NCM
interface (e.g. `en8`) with a self-assigned `169.254.x.x` address. The receiver
already detects this link and advertises it as `usbIP` in its Bonjour TXT
record (`TBReceiverC/src/main.c`), and the URL-scheme automation can already
probe and select a `.usb` path (`TBConnectionDiagnostics.pathKind`).

The sender GUI, however, cannot select it:

1. `TBTransportKind` only has `thunderboltBridge` and `networkLink`.
2. `TBDisplaySenderService.detectLocalInterfaces()` accepts `169.254.*` only on
   `bridge*` interfaces; every other interface goes through `isLikelyLANIPv4`,
   which rejects `169.254.*`. A USB-NCM interface is therefore silently dropped.
3. `connect()` pins the source address (`requiredLocalEndpoint`) but not the
   egress interface. macOS routes `169.254/16` via the primary interface, so a
   USB-NCM dial can leave via Wi-Fi and time out. `scopedReceiverHost` only adds
   a `%bridge0` zone for Thunderbolt; upstream deliberately does not zone-scope
   USB-NCM (`testUSBNCMLinkLocalReceiverIsNotGivenAnIPv4ZoneSuffix`).

## Goals

- A user can pick **Direct USB** as the transport, pick the USB-NCM local
  interface, and connect to the receiver's `usbIP`.
- The connection reliably egresses over the USB link even when Wi-Fi is up.
- Automation `transport=usb` (and aliases) selects the new transport.
- No behaviour change for existing Thunderbolt Bridge / Network Link sessions.

## Non-goals

- Receiver changes (it already advertises `usbIP`).
- Replacing manual transport selection with GUI auto-probing (possible later PR).
- IPv6 link-local support.

## Design

### 1. Model and interface detection

- Add `case directUSB` to `TBTransportKind`, declared between
  `thunderboltBridge` and `networkLink`. Title: "Direct USB" in all five
  languages (it, en, de, fr, zh — zh: "USB 直连"). Raw value `directUSB` is what
  gets persisted; existing configs decode unchanged, and an older build reading
  `directUSB` falls back to its default via the existing
  `TBTransportKind(rawValue:)` guard.
- Extract interface classification from the private `getifaddrs` loop into a
  pure, testable `static func TBConnectionDiagnostics.transportKind(forInterfaceName:ip:) -> TBTransportKind?`
  (in the shared file, next to `isDirectLinkInterface`; it replaces the
  sender's private `isLikelyLANIPv4`, which matches `isPrivateIPv4`):
  - `bridge*` + `169.254.*` → `.thunderboltBridge` (unchanged)
  - `TBConnectionDiagnostics.isDirectLinkInterface(name:ip:)` (i.e. `en*`/`eth*`
    + valid `169.254.*`) → `.directUSB`
  - existing LAN name/IP heuristics → `.networkLink` (unchanged)
  - otherwise `nil`
  Reusing `isDirectLinkInterface` keeps the GUI and automation on one classifier.
  Accepted consequence: an Ethernet port that fell back to link-local (no DHCP)
  also appears as a Direct USB candidate; its interface name in the picker
  label makes this visible, and the connection simply fails if no peer exists.
- `TBConnectionDiagnostics` path-kind → transport mapping
  (`TBConnectionDiagnostics.swift:52`): `.usb` → `.directUSB`,
  `.thunderbolt` → `.thunderboltBridge`, everything else → `.networkLink`.

### 2. Receiver address and connect pinning

- `TBDiscoveredReceiver.ip(for: .directUSB)` → `usbIP`, falling back to
  `preferredIP` when empty. The existing `.networkLink` +
  `169.254` local IP → `usbIP` mapping stays for older saved sessions.
- New `TBNetworkInterfaceCache` (in `TBConnectionDiagnostics.swift`): one
  long-lived `NWPathMonitor`, started from `refreshLocalInterfaces()`, that
  caches `availableInterfaces`; `interface(named:) -> NWInterface?` looks up by
  name. A one-shot monitor would not work: it has no interfaces until its
  first asynchronous update, and Network.framework has no lookup-by-name API.
- In `TBDisplaySenderSession.connect()`, after `connectInterfaceName` is
  resolved: when `transportKind` is `.directUSB` or `.thunderboltBridge` and an
  `NWInterface` matches, set `params.requiredInterface`. Log the pinned
  interface in the existing `connect: dialing …` line.
- `scopedReceiverHost` and its tests are left untouched; interface pinning is
  an additional, independent guarantee.

### 3. UI, automation, diagnostics

- Transport picker in `TBDisplaySenderContentView` iterates
  `TBTransportKind.allCases`, so Direct USB appears automatically; the local
  interface picker already filters by `availableInterfaces(for:)`. Extend the
  transport help text (5 languages) with one sentence on Direct USB.
- `TBDisplaySenderService.availableTransportKinds` returns `.directUSB`
  unconditionally. Unlike Network Link it is not gated behind an addon
  capability; it is a direct cable like Thunderbolt Bridge.
  (`TBDisplaySenderSettingsView`'s `.networkLink` rows are addon
  capabilities, not transports, and stay untouched.)
- `TBSenderAutomation.parseTransport`: `usb`, `usb4`, `usbc`, `directusb`
  (case-insensitive, `-`/`_`/space stripped, matching
  `TBConnectionPathPreference.parse`) → `.directUSB`.
- `TBConfigurationDiagnosticSnapshot` gains
  `var transportIsDirectUSB: Bool = false` (defaulted so existing call sites and
  tests compile unchanged). `localLinkCheck` validates the interface against
  the transport:
  - TB: `bridge*` expected (unchanged keys)
  - Direct USB: `isDirectLinkInterface` expected →
    `sender.diagnostics.usb_link_ready` / `sender.diagnostics.usb_link_unexpected`
  - Network Link: unchanged.
  New keys added to `TargetBridge-Shared/Languages/{it,en,de,fr,zh}.json`
  (where the existing `sender.diagnostics.*` keys live).
- `docs/Automation.md`: document `transport=usb`.

### 4. Error handling

- No matching `NWInterface` at connect time (cable just unplugged, race with
  interface teardown): log a warning and dial unpinned. The existing connect
  timeout and `failureDetail` surface the real network state; no new silent
  failure paths.
- `availableInterfaces(for: .directUSB)` empty: same UX as an empty
  Thunderbolt list today (Connect disabled, diagnostics shows
  `local_link_missing`).

## Testing

XCTest in `TargetBridge-Sender/TBDisplaySenderTests`, following existing files:

- Interface classification: `en8 169.254.190.84` → `.directUSB`;
  `bridge0 169.254.109.86` → `.thunderboltBridge`; `en0 192.168.1.225` →
  `.networkLink`; `en0 169.254.3.4` → `.directUSB`; `utun3 10.0.0.1` → `nil`.
- `TBDiscoveredReceiver.ip(for: .directUSB)` with and without `usbIP`.
- `parseTransport` for every USB alias, plus existing values unchanged.
- Path-kind → transport mapping for all `TBConnectionPathKind` cases.
- `TBConfigurationDiagnostics.checks` local-link row for Direct USB with a
  matching and a non-matching interface.

Manual (not unit-testable — requires two Macs and a USB-C/USB4 cable),
added to `docs/Testing.md`:

1. Connect the Macs with a USB-C cable, keep Wi-Fi on on both.
2. Receiver shows `Direct USB-NCM IP = 169.254.x.y`.
3. Sender: choose **Direct USB**, pick the `en*` · `169.254.*` interface, Connect.
4. Confirm the log line shows the pinned interface and the stream reaches
   `streaming`; run the cable test and confirm USB-class throughput.
5. Unplug the cable mid-session; confirm the existing reconnect/failure path.

## Files touched

- `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderManager.swift`
- `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderService.swift`
- `TargetBridge-Sender/TBDisplaySender/TBReceiverDiscovery.swift`
- `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderAutomation.swift`
- `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderContentView.swift`
- `TargetBridge-Sender/TBDisplaySender/TBConfigurationDiagnostics.swift`
- `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderLocalization.swift` (transport help text, if needed)
- `TargetBridge-Shared/Languages/*.json` (diagnostics keys)
- `TargetBridge-Sender/TBDisplayShared/TBConnectionDiagnostics.swift`
- `TargetBridge-Sender/TBDisplaySenderTests/*` (new/extended tests)
- `docs/Automation.md`, `docs/Testing.md`

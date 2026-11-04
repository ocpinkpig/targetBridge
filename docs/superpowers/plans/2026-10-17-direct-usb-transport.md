# Direct USB Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the TargetBridge sender select a direct USB-C/USB4 (USB-NCM, `en*` + `169.254.x.x`) link as its transport and reliably connect over it.

**Architecture:** Add `TBTransportKind.directUSB`; move interface→transport classification into `TBConnectionDiagnostics` (shared with automation) so `en*`/`eth*` link-local interfaces become Direct USB candidates; pin the egress interface with `NWParameters.requiredInterface` (via a long-lived `NWPathMonitor` cache) for Direct USB and Thunderbolt dials; wire automation, diagnostics and copy.

**Tech Stack:** Swift / SwiftUI / Network.framework, XCTest, XcodeGen project (`TargetBridge-Sender/project.yml`).

**Spec:** `docs/superpowers/specs/2026-10-17-direct-usb-transport-design.md`

## Global Constraints

- Upstream style: every user-visible string in all five languages — italian, english, german, french, chinese — in the same `switch (case, language)` form the file already uses.
- Transport title copy: it/en/de/fr "Direct USB", zh "USB 直连".
- Raw value of the new case is exactly `directUSB` (persisted in session config).
- Diagnostics strings live in `TargetBridge-Shared/Languages/{it,en,de,fr,zh}.json`.
- No receiver (`TargetBridge-Receiver`) changes.
- No new source files: new types go into existing files so `project.pbxproj` needs no regeneration.
- Direct USB is **not** gated behind an addon capability (unlike Network Link) — it is a direct cable like Thunderbolt Bridge.
- Test command (run from `TargetBridge-Sender/`):
  `xcodebuild test -project TargetBridge.xcodeproj -scheme TBDisplaySender -destination 'platform=macOS' -only-testing:TBDisplaySenderTests/<Class>/<method>`
  Full suite: same command without `-only-testing`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. Wi-Fi up with its own `169.254` self-assigned address next to USB-NCM — expected: connection still leaves via the USB interface (Task 3 pinning; manual check in Task 6).
2. Ethernet port without DHCP (`en0 169.254.x`) — expected: listed as a Direct USB candidate, labelled `en0`, never as Network Link (Task 2 test `testEthernetWithoutDHCPIsDirectUSBCandidate`).
3. Cable unplugged between interface refresh and Connect — expected: `requiredInterface` lookup returns nil, a warning is logged, dial proceeds unpinned and fails through the existing timeout (Task 3 test `testPinningIsSkippedWhenInterfaceIsUnknown`).
4. Saved session with `transportKind = networkLink` and a `169.254` local IP from before this change — expected: keeps working and still resolves to `usbIP` (Task 1 test `testLegacyNetworkLinkLinkLocalStillResolvesUSBIP`).
5. Automation `transport=usb` while the receiver advertises no `usbIP` — expected: falls back to `preferredIP` rather than an empty address (Task 1 test `testDirectUSBFallsBackToPreferredIP`).

---

### Task 1: `directUSB` transport kind and receiver addressing

**Files:**
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderManager.swift:7-27` (enum + title), `:212-221` (`availableTransportKinds`)
- Modify: `TargetBridge-Sender/TBDisplaySender/TBReceiverDiscovery.swift:91-101` (`ip(for:localInterfaceIP:)`)
- Modify: `TargetBridge-Sender/TBDisplayShared/TBConnectionDiagnostics.swift:51-53` (`TBConnectionPathKind.transportKind`)
- Test: `TargetBridge-Sender/TBDisplaySenderTests/TBReceiverDiscoveryModelTests.swift`, `TargetBridge-Sender/TBDisplaySenderTests/TBConnectionDiagnosticsTests.swift`

**Interfaces:**
- Produces: `TBTransportKind.directUSB` (raw `"directUSB"`), declared between `thunderboltBridge` and `networkLink`; `TBConnectionPathKind.usb.transportKind == .directUSB`.

- [ ] **Step 1: Write failing tests**

In `TBReceiverDiscoveryModelTests` (use the existing `makeReceiver` helper):

```swift
func testDirectUSBUsesAdvertisedUSBIP() {
    let receiver = makeReceiver(preferredIP: "192.168.1.64", usbIP: "169.254.189.3", networkIP: "192.168.1.64")
    XCTAssertEqual(receiver.ip(for: .directUSB), "169.254.189.3")
}

func testDirectUSBFallsBackToPreferredIP() {
    let receiver = makeReceiver(preferredIP: "169.254.89.80", usbIP: "")
    XCTAssertEqual(receiver.ip(for: .directUSB), "169.254.89.80")
}

func testLegacyNetworkLinkLinkLocalStillResolvesUSBIP() {
    let receiver = makeReceiver(preferredIP: "192.168.1.64", usbIP: "169.254.189.3", networkIP: "192.168.1.64")
    XCTAssertEqual(receiver.ip(for: .networkLink, localInterfaceIP: "169.254.190.84"), "169.254.189.3")
}
```

In `TBConnectionDiagnosticsTests`:

```swift
func testPathKindMapsToTransportKind() {
    XCTAssertEqual(TBConnectionPathKind.thunderbolt.transportKind, .thunderboltBridge)
    XCTAssertEqual(TBConnectionPathKind.usb.transportKind, .directUSB)
    XCTAssertEqual(TBConnectionPathKind.ethernet.transportKind, .networkLink)
    XCTAssertEqual(TBConnectionPathKind.wifi.transportKind, .networkLink)
}
```

- [ ] **Step 2: Run to verify failure**

Run the four tests with `-only-testing`. Expected: build fails — `type 'TBTransportKind' has no member 'directUSB'`.

- [ ] **Step 3: Implement**

- Add `case directUSB` with the five `title(_:)` rows from Global Constraints.
- `ip(for:)`: `.directUSB` → `usbIP` if non-empty else `preferredIP`. Leave the `.networkLink` branch unchanged.
- `availableTransportKinds`: `.directUSB` → `true`.
- `TBConnectionPathKind.transportKind`: `switch` returning `.thunderboltBridge` / `.directUSB` / `.networkLink` for ethernet and wifi.
- Fix any other exhaustive `switch` the compiler flags the same way.

- [ ] **Step 4: Run tests — expected PASS** (the four new tests plus the existing `TBReceiverDiscoveryModelTests` and `TBConnectionDiagnosticsTests` classes).

- [ ] **Step 5: Commit** — `feat(sender): add Direct USB transport kind`

---

### Task 2: Classify USB-NCM interfaces as Direct USB

**Files:**
- Modify: `TargetBridge-Sender/TBDisplayShared/TBConnectionDiagnostics.swift` (new static func next to `isDirectLinkInterface`, ≈ line 130)
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderManager.swift:939-1012` (`detectLocalInterfaces`, delete `isLikelyLocalNetworkInterfaceName` and `isLikelyLANIPv4`)
- Test: `TargetBridge-Sender/TBDisplaySenderTests/TBConnectionDiagnosticsTests.swift`

**Interfaces:**
- Consumes: `TBTransportKind.directUSB` (Task 1).
- Produces: `static func TBConnectionDiagnostics.transportKind(forInterfaceName name: String, ip: String) -> TBTransportKind?`

- [ ] **Step 1: Write failing tests**

```swift
func testInterfaceClassificationByTransport() {
    let cases: [(String, String, TBTransportKind?)] = [
        ("bridge0", "169.254.109.86", .thunderboltBridge),
        ("en8", "169.254.190.84", .directUSB),
        ("eth2", "169.254.1.2", .directUSB),
        ("en0", "192.168.1.225", .networkLink),
        ("en0", "10.0.0.5", .networkLink),
        ("en0", "172.16.0.1", .networkLink),
        ("en0", "172.31.255.1", .networkLink),
        ("bridge100", "192.168.2.1", .networkLink),
        ("en0", "172.32.0.1", nil),
        ("en0", "8.8.8.8", nil),
        ("utun3", "10.0.0.1", nil),
        ("awdl0", "169.254.3.3", nil),
        ("lo0", "127.0.0.1", nil),
        ("en8", "169.254.300.1", nil),
    ]
    for (name, ip, expected) in cases {
        XCTAssertEqual(TBConnectionDiagnostics.transportKind(forInterfaceName: name, ip: ip), expected, "\(name) \(ip)")
    }
}

func testEthernetWithoutDHCPIsDirectUSBCandidate() {
    XCTAssertEqual(TBConnectionDiagnostics.transportKind(forInterfaceName: "en0", ip: "169.254.3.4"), .directUSB)
}
```

- [ ] **Step 2: Run to verify failure** — expected: build fails, `transportKind(forInterfaceName:ip:)` not found.

- [ ] **Step 3: Implement**

Rules, in order: `bridge*` + `isIPv4LinkLocal` → `.thunderboltBridge`; `isDirectLinkInterface(name:ip:)` → `.directUSB`; name has prefix `en`/`eth`/`bridge` and `isPrivateIPv4(ip)` → `.networkLink`; else `nil`. (`isPrivateIPv4` matches the deleted `isLikelyLANIPv4` exactly.) In `detectLocalInterfaces`, replace the two branches after `let ip = …` with one call: append when the result is non-nil.

- [ ] **Step 4: Run tests — expected PASS** (new tests + whole `TBConnectionDiagnosticsTests`).

- [ ] **Step 5: Commit** — `feat(sender): list USB-NCM link-local interfaces as Direct USB`

---

### Task 3: Pin the egress interface on connect

**Files:**
- Modify: `TargetBridge-Sender/TBDisplayShared/TBConnectionDiagnostics.swift` (new `TBNetworkInterfaceCache` type at the end of the file; new static func)
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderService.swift:1595-1640` (`TBDisplaySenderSession.connect()`)
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderManager.swift:515` (`refreshLocalInterfaces` starts the cache)
- Test: `TargetBridge-Sender/TBDisplaySenderTests/TBConnectionDiagnosticsTests.swift`

**Interfaces:**
- Consumes: `TBTransportKind.directUSB` (Task 1).
- Produces:
  - `static func TBConnectionDiagnostics.shouldPinEgressInterface(transport: TBTransportKind, interfaceName: String?) -> Bool` — true when `transport` is `.directUSB` or `.thunderboltBridge` and `interfaceName` is non-nil and non-empty.
  - `final class TBNetworkInterfaceCache: @unchecked Sendable` with `static let shared`, `func start()` (idempotent; starts one `NWPathMonitor` on a private serial queue and stores `path.availableInterfaces` under an `NSLock` on every update) and `func interface(named: String) -> NWInterface?`.

- [ ] **Step 1: Write failing tests**

```swift
func testPinsEgressForDirectLinkTransports() {
    XCTAssertTrue(TBConnectionDiagnostics.shouldPinEgressInterface(transport: .directUSB, interfaceName: "en8"))
    XCTAssertTrue(TBConnectionDiagnostics.shouldPinEgressInterface(transport: .thunderboltBridge, interfaceName: "bridge0"))
    XCTAssertFalse(TBConnectionDiagnostics.shouldPinEgressInterface(transport: .networkLink, interfaceName: "en0"))
}

func testPinningIsSkippedWhenInterfaceIsUnknown() {
    XCTAssertFalse(TBConnectionDiagnostics.shouldPinEgressInterface(transport: .directUSB, interfaceName: nil))
    XCTAssertFalse(TBConnectionDiagnostics.shouldPinEgressInterface(transport: .directUSB, interfaceName: ""))
}
```

- [ ] **Step 2: Run to verify failure** — build fails, symbol not found.

- [ ] **Step 3: Implement**

- Add `shouldPinEgressInterface` and `TBNetworkInterfaceCache` as specified.
- `refreshLocalInterfaces()` calls `TBNetworkInterfaceCache.shared.start()` first.
- In `connect()`, after `connectInterfaceName` is set and before `NWConnection` is created: if `shouldPinEgressInterface(...)`, look up `TBNetworkInterfaceCache.shared.interface(named:)`. If found, set `params.requiredInterface`; if not, `TBLog.connection.warning("connect: no NWInterface named …; dialing unpinned")`.
- Add `pinned=<name|none>` to the existing `connect: dialing …` log line.
- Do not touch `scopedReceiverHost` or its tests.

- [ ] **Step 4: Run full `TBConnectionDiagnosticsTests` — expected PASS** (including the unchanged `testUSBNCMLinkLocalReceiverIsNotGivenAnIPv4ZoneSuffix`).

- [ ] **Step 5: Commit** — `fix(sender): pin Direct USB and Thunderbolt dials to their interface`

---

### Task 4: Automation and CLI accept `usb`

**Files:**
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderAutomation.swift:513-520` (`parseTransport`)
- Modify: `cli/targetbridge:13` (usage line), `docs/Automation.md:26` (options line)
- Test: `TargetBridge-Sender/TBDisplaySenderTests/TBSenderAutomationParsingTests.swift:345-359`

**Interfaces:**
- Consumes: `TBTransportKind.directUSB`.

- [ ] **Step 1: Write failing test**

```swift
func testParseTransportUSBAliases() {
    for alias in ["usb", "USB", "usb4", "USB-C", "usbc", "directusb", "Direct USB", "direct_usb"] {
        XCTAssertEqual(TBSenderAutomation.parseTransport(alias), .directUSB, "alias \(alias)")
    }
}
```

Existing `testParseTransportNetworkAliases` and `testParseTransportDefaultsToThunderbolt` stay unchanged and must keep passing.

- [ ] **Step 2: Run to verify failure** — expected: assertion fails (`thunderboltBridge` ≠ `directUSB`).

- [ ] **Step 3: Implement** — normalise like `TBConnectionPathPreference.parse` does (lowercase, strip space, `-` and `_`) before the `switch`; add `case "usb", "usb4", "usbc", "directusb": return .directUSB`. Update the CLI usage and docs option text to `--transport tb|usb|net`.

- [ ] **Step 4: Run `TBSenderAutomationParsingTests` — expected PASS.**

- [ ] **Step 5: Commit** — `feat(automation): accept transport=usb for Direct USB`

---

### Task 5: Diagnostics check for the Direct USB link

**Files:**
- Modify: `TargetBridge-Sender/TBDisplaySender/TBConfigurationDiagnostics.swift:17-35` (snapshot), `:49-59` (`localLinkCheck`)
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderManager.swift:258` (snapshot construction)
- Modify: `TargetBridge-Shared/Languages/{it,en,de,fr,zh}.json` (next to `sender.diagnostics.thunderbolt_link_ready`)
- Test: `TargetBridge-Sender/TBDisplaySenderTests/TBConfigurationDiagnosticsTests.swift`

**Interfaces:**
- Consumes: `TBConnectionDiagnostics.isDirectLinkInterface(name:ip:)` is IP-based. The snapshot only has a name, so this check uses a name rule: `en*`/`eth*`.
- Produces: `TBConfigurationDiagnosticSnapshot.transportIsDirectUSB: Bool = false`; keys `sender.diagnostics.usb_link_ready`, `sender.diagnostics.usb_link_unexpected` (both take `%{interface}`).

- [ ] **Step 1: Write failing tests**

Extend `baseSnapshot` with `transportIsThunderbolt: Bool = true, transportIsDirectUSB: Bool = false` as its second and third parameters (before `localInterfaceName`), passing them through. Declare the new snapshot field right after `transportIsThunderbolt` so the memberwise init's argument order matches. Then:

```swift
func testDirectUSBTransportPassesOnEnInterface() {
    let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(transportIsThunderbolt: false, transportIsDirectUSB: true, localInterfaceName: "en8"))
    let link = checks.first(where: { $0.id == "local_link" })
    XCTAssertEqual(link?.state, .passed)
    XCTAssertEqual(link?.detailKey, "sender.diagnostics.usb_link_ready")
}

func testDirectUSBTransportWarnsOnBridgeInterface() {
    let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(transportIsThunderbolt: false, transportIsDirectUSB: true, localInterfaceName: "bridge0"))
    let link = checks.first(where: { $0.id == "local_link" })
    XCTAssertEqual(link?.state, .attention)
    XCTAssertEqual(link?.detailKey, "sender.diagnostics.usb_link_unexpected")
}
```

- [ ] **Step 2: Run to verify failure** — build fails on the unknown `transportIsDirectUSB` argument.

- [ ] **Step 3: Implement**

- Add the defaulted field.
- In `localLinkCheck`, branch on `transportIsDirectUSB` before the Thunderbolt branch: `en`/`eth` prefix → `.passed` with `usb_link_ready`, otherwise `.attention` with `usb_link_unexpected`.
- Manager passes `transportIsDirectUSB: session.transportKind == .directUSB`.
- en JSON copy: `"Direct USB is selected on %{interface}."` and `"Direct USB expects a USB network interface (en…), but %{interface} is selected."`. Translate into it/de/fr/zh in the tone of the neighbouring keys.

- [ ] **Step 4: Run `TBConfigurationDiagnosticsTests` — expected PASS** (the existing Thunderbolt test unchanged).

- [ ] **Step 5: Commit** — `feat(diagnostics): validate the Direct USB local link`

---

### Task 6: Picker copy, manual test doc, full verification

**Files:**
- Modify: `TargetBridge-Sender/TBDisplaySender/TBDisplaySenderContentView.swift:1025-1033` (`transportDetails`), `:598` (receiver IP placeholder unchanged — already `169.254.x.x`)
- Modify: `docs/Testing.md` (new "Direct USB (manual)" section)

- [ ] **Step 1: Update `transportDetails`** — append one sentence per language. en: `" Direct USB uses a USB-C/USB4 cable between the two Macs."` it: `" Direct USB usa un cavo USB-C/USB4 tra i due Mac."` de: `" Direct USB nutzt ein USB-C/USB4-Kabel zwischen den beiden Macs."` fr: `" Direct USB utilise un câble USB-C/USB4 entre les deux Mac."` zh: `"USB 直连使用两台 Mac 之间的 USB-C/USB4 线缆。"`

- [ ] **Step 2: Add the manual checklist to `docs/Testing.md`** — the five steps from the spec's Testing section, verbatim. In step 4, add: look for `pinned=en…` in the `connect: dialing` log line (`log stream --predicate 'subsystem CONTAINS "TargetBridge"'`).

- [ ] **Step 3: Full suite**

Run: `xcodebuild test -project TargetBridge.xcodeproj -scheme TBDisplaySender -destination 'platform=macOS'` from `TargetBridge-Sender/`.
Expected: `** TEST SUCCEEDED **`.

- [ ] **Step 4: Build the app** — `TargetBridge-Sender/scripts/build_targetbridge_sender_app.sh` (or the scheme build) succeeds, and the transport picker lists "Direct USB".

- [ ] **Step 5: Commit** — `docs: Direct USB picker copy and manual test checklist`

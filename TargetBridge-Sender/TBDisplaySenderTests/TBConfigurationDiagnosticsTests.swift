import XCTest
@testable import TargetBridge

final class TBConfigurationDiagnosticsTests: XCTestCase {
    func testMissingEssentialsNeedAttention() {
        let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(hasScreenRecording: false, localInterfaceName: nil, receiverAddress: ""))

        XCTAssertEqual(checks.first(where: { $0.id == "screen_recording" })?.state, .attention)
        XCTAssertEqual(checks.first(where: { $0.id == "local_link" })?.state, .attention)
        XCTAssertEqual(checks.first(where: { $0.id == "receiver_address" })?.state, .attention)
    }

    func testThunderboltTransportWarnsWhenBridgeIsNotSelected() {
        let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(localInterfaceName: "en0"))
        XCTAssertEqual(checks.first(where: { $0.id == "local_link" })?.state, .attention)
    }

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

    func testReceiverControlChecksBothSidesOfInputRelay() {
        let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(requiresSenderAccessibility: true, senderAccessibilityGranted: false, requiresReceiverInputMonitoring: true, receiverInputMonitoringGranted: false))

        XCTAssertEqual(checks.first(where: { $0.id == "sender_accessibility" })?.state, .attention)
        XCTAssertEqual(checks.first(where: { $0.id == "receiver_input_monitoring" })?.state, .attention)
    }

    func testDirectUSBLinkRejectsEthernetNameOnPrivateNetworkIP() {
        let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(transportIsThunderbolt: false, transportIsDirectUSB: true, localInterfaceName: "en0", localInterfaceIP: "192.168.1.5"))
        let link = checks.first(where: { $0.id == "local_link" })
        XCTAssertEqual(link?.state, .attention)
        XCTAssertEqual(link?.detailKey, "sender.diagnostics.usb_link_unexpected")
    }

    func testDirectUSBLinkAcceptsLinkLocalIP() {
        let checks = TBConfigurationDiagnostics.checks(for: baseSnapshot(transportIsThunderbolt: false, transportIsDirectUSB: true, localInterfaceName: "en8", localInterfaceIP: "169.254.190.84"))
        let link = checks.first(where: { $0.id == "local_link" })
        XCTAssertEqual(link?.state, .passed)
        XCTAssertEqual(link?.detailKey, "sender.diagnostics.usb_link_ready")
    }

    private func baseSnapshot(
        hasScreenRecording: Bool = true,
        transportIsThunderbolt: Bool = true,
        transportIsDirectUSB: Bool = false,
        localInterfaceName: String? = "bridge0",
        localInterfaceIP: String? = nil,
        receiverAddress: String = "169.254.1.2",
        requiresSenderAccessibility: Bool = false,
        senderAccessibilityGranted: Bool = true,
        requiresReceiverInputMonitoring: Bool = false,
        receiverInputMonitoringGranted: Bool? = true
    ) -> TBConfigurationDiagnosticSnapshot {
        TBConfigurationDiagnosticSnapshot(
            hasScreenRecording: hasScreenRecording, transportIsThunderbolt: transportIsThunderbolt, transportIsDirectUSB: transportIsDirectUSB,
            localInterfaceName: localInterfaceName,
            localInterfaceIP: localInterfaceIP,
            receiverAddress: receiverAddress, receiverProfileAvailable: true, receiverSupportsHEVC: true,
            requiresHEVC: false, cableRate: 18.5, requiresSenderInputMonitoring: false,
            senderInputMonitoringGranted: true, requiresSenderAccessibility: requiresSenderAccessibility,
            senderAccessibilityGranted: senderAccessibilityGranted, requiresReceiverInputMonitoring: requiresReceiverInputMonitoring,
            receiverInputMonitoringGranted: receiverInputMonitoringGranted, requiresReceiverAccessibility: false,
            receiverAccessibilityGranted: true
        )
    }
}

// haptics_probe -- find a way to buzz a DualSense from macOS (game track research).
//
// The game's Core Haptics path failed in play with "Couldn't communicate with a helper
// application" when it created a pattern player. This probe tries the variants one by one,
// prints what macOS accepts, and buzzes for ~1 s on each that works, so a person holding the pad
// can say which ones they felt. Run: swiftc -O main.swift -o /tmp/haptics_probe && /tmp/haptics_probe
import CoreHaptics
import Foundation
import GameController

func log(_ s: String) { print(s); fflush(stdout) }

func waitForController(_ seconds: Double) -> GCController? {
    GCController.startWirelessControllerDiscovery {}
    let end = Date().addingTimeInterval(seconds)
    while Date() < end {
        if let c = GCController.controllers().first { return c }
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
    }
    return GCController.controllers().first
}

func run(_ name: String, _ body: () throws -> Void) {
    log("--- \(name)")
    do { try body(); log("    OK (did you feel it?)") } catch { log("    FAIL: \(error.localizedDescription) [\(error)]") }
    RunLoop.current.run(until: Date().addingTimeInterval(1.5))
}

guard let pad = waitForController(8) else { log("no controller found (is the DualSense on and paired?)"); exit(1) }
log("controller: \(pad.vendorName ?? "?") category \(pad.productCategory)")
guard let haptics = pad.haptics else { log("this controller reports NO haptics on this Mac"); exit(2) }
log("haptics localities: \(haptics.supportedLocalities.map { $0.rawValue }.sorted())")

func event(_ type: CHHapticEvent.EventType, _ intensity: Float, _ duration: TimeInterval) -> CHHapticEvent {
    CHHapticEvent(eventType: type, parameters: [
        CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
        CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.1),
    ], relativeTime: 0, duration: duration)
}

for locality in [GCHapticsLocality.default, GCHapticsLocality.handles] where haptics.supportedLocalities.contains(locality) {
    guard let engine = haptics.createEngine(withLocality: locality) else { log("no engine for \(locality.rawValue)"); continue }
    run("[\(locality.rawValue)] engine.start") { try engine.start() }
    run("[\(locality.rawValue)] plain player, continuous 1.0 s") {
        let p = try engine.makePlayer(with: CHHapticPattern(events: [event(.hapticContinuous, 1.0, 1.0)], parameters: []))
        try p.start(atTime: CHHapticTimeImmediate)
    }
    run("[\(locality.rawValue)] advanced player, continuous 1.0 s") {
        let p = try engine.makeAdvancedPlayer(with: CHHapticPattern(events: [event(.hapticContinuous, 1.0, 1.0)], parameters: []))
        try p.start(atTime: CHHapticTimeImmediate)
    }
    run("[\(locality.rawValue)] advanced player, continuous 30 s, looped (the game's way), stop after 1 s") {
        let p = try engine.makeAdvancedPlayer(with: CHHapticPattern(events: [event(.hapticContinuous, 1.0, 30.0)], parameters: []))
        p.loopEnabled = true
        try p.start(atTime: CHHapticTimeImmediate)
        RunLoop.current.run(until: Date().addingTimeInterval(1.0))
        try p.stop(atTime: CHHapticTimeImmediate)
    }
    run("[\(locality.rawValue)] transient taps x5") {
        var evs: [CHHapticEvent] = []
        for i in 0..<5 { evs.append(CHHapticEvent(eventType: .hapticTransient, parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: 1.0)], relativeTime: Double(i) * 0.15)) }
        try engine.makePlayer(with: CHHapticPattern(events: evs, parameters: [])).start(atTime: CHHapticTimeImmediate)
    }
    engine.stop(completionHandler: nil)
}
log("done")

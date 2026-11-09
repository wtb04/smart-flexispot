import Foundation

/// Where the desk stands, as the panel says in its pong and in its answer to a
/// tap; Stand and Sit are also its presets, where it goes.
enum Desk: String {
    case stand, sit, between

    static let presets: [Desk] = [.stand, .sit]

    enum Look { case plain, lit, ringed }

    /// Nil without a desk, as from a panel before this Desk Link or one with no desk linked.
    static func said(_ answer: [String: Any]) -> (at: Desk?, going: Desk?, heightMM: Int?) {
        guard let at = (answer["desk"] as? String).flatMap(Desk.init(rawValue:)) else { return (nil, nil, nil) }
        let going = (answer["going"] as? String).flatMap(Desk.init(rawValue:))
        // a number, not JSON's true, which NSNumber would also take for 1
        let height = (answer["height"] as? NSNumber).flatMap { CFGetTypeID($0) == CFBooleanGetTypeID() ? nil : $0.intValue }
        return (at, going == .between ? nil : going, height.flatMap { (0..<3000).contains($0) ? $0 : nil })
    }

    /// As the panel shows it, "74.0 cm".
    static func height(_ mm: Int) -> String { String(format: "%.1f cm", Double(mm) / 10) }

    /// This preset's button: lit where the desk stands, ringed where it goes.
    func look(at: Desk?, going: Desk?) -> Look {
        if at == self { return .lit }
        if going == self { return .ringed }
        return .plain
    }
}

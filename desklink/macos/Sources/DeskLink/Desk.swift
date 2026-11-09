/// Where the desk stands, as the panel says in its pong and in its answer to a
/// tap; Stand and Sit are also its presets, where it goes.
enum Desk: String {
    case stand, sit, between

    static let presets: [Desk] = [.stand, .sit]

    enum Look { case plain, lit, ringed }

    /// Nil without a desk, as from a panel before this Desk Link or one with no desk linked.
    static func said(_ answer: [String: Any]) -> (at: Desk?, going: Desk?) {
        guard let at = (answer["desk"] as? String).flatMap(Desk.init(rawValue:)) else { return (nil, nil) }
        let going = (answer["going"] as? String).flatMap(Desk.init(rawValue:))
        return (at, going == .between ? nil : going)
    }

    /// This preset's button: lit where the desk stands, ringed where it goes.
    func look(at: Desk?, going: Desk?) -> Look {
        if at == self { return .lit }
        if going == self { return .ringed }
        return .plain
    }
}

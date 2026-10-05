import CryptoKit
import Foundation

/// The envelope every message to and from the panel goes in, as the panel's
/// components/laptop/include/link_envelope.h has it: "DL1", a 12-byte nonce,
/// the AES-256-GCM ciphertext and its tag, authenticated with "DL1 " and the
/// path it goes to. Inside is JSON with "type" and "at", in Unix milliseconds.
struct Seal {
    static let window: Double = 60  // seconds either way a message may be off by
    private static let magic = Data("DL1".utf8)

    private let key: SymmetricKey
    private var seen: [Data] = []

    /// Nil unless `hex` is 32 bytes of hex.
    init?(hex: String) {
        guard hex.count == 64 else { return nil }
        var bytes = [UInt8]()
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            bytes.append(byte)
            index = next
        }
        key = SymmetricKey(data: bytes)
    }

    func seal(_ plain: Data, to path: String) -> Data? {
        guard let box = try? AES.GCM.seal(plain, using: key, authenticating: Data("DL1 \(path)".utf8)),
              let combined = box.combined else { return nil }
        return Self.magic + combined
    }

    func seal(_ message: [String: Any], to path: String) -> Data? {
        var message = message
        message["at"] = Self.now
        guard let plain = try? JSONSerialization.data(withJSONObject: message) else { return nil }
        return seal(plain, to: path)
    }

    /// The plain bytes, or nil when they are not ours: another key, or changed on the way.
    func open(_ data: Data, from path: String) -> (plain: Data, nonce: Data)? {
        guard data.count > Self.magic.count + 12 + 16, data.prefix(Self.magic.count) == Self.magic,
              let box = try? AES.GCM.SealedBox(combined: data.dropFirst(Self.magic.count)),
              let plain = try? AES.GCM.open(box, using: key, authenticating: Data("DL1 \(path)".utf8))
        else { return nil }
        return (plain, Data(box.nonce))
    }

    /// A message opened, of its type and fresh, and not seen before; nil otherwise.
    mutating func message(_ data: Data, from path: String) -> [String: Any]? {
        guard let (plain, nonce) = open(data, from: path),
              let message = try? JSONSerialization.jsonObject(with: plain) as? [String: Any],
              Self.fresh(message), !seen.contains(nonce) else { return nil }
        seen.append(nonce)
        if seen.count > 64 { seen.removeFirst() }
        return message
    }

    static func fresh(_ message: [String: Any]) -> Bool {
        guard let at = (message["at"] as? NSNumber)?.doubleValue else { return false }
        return abs(at / 1000 - Date().timeIntervalSince1970) <= window
    }

    static var now: Int64 { Int64(Date().timeIntervalSince1970 * 1000) }
}

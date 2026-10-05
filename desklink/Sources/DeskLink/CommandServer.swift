import Darwin
import Foundation

struct Request {
    let method: String
    let path: String
    let headers: [String: String]  // names in lower case
    let body: Data
    var local = false  // from this Mac itself
}

struct Response {
    var status: Int
    var type = ""
    var body = Data()

    static let noContent = Response(status: 204)
    static let notFound = Response(status: 404)
}

/// Where the panel sends its commands and fetches the cover: one request to a
/// connection, as the panel closes it after. On the kernel's own sockets, as
/// curl is: Network.framework's TCP stalled on each of the panel's small
/// receive windows, and a cover took seconds where curl takes milliseconds.
final class CommandServer {
    private let port: UInt16
    private let handle: (Request) -> Response
    private let queue = DispatchQueue(label: "nl.w-tb.desklink.server", attributes: .concurrent)
    private var listening: Int32 = -1

    private static let largest = 64 * 1024
    private static let patience = timeval(tv_sec: 5, tv_usec: 0)

    init(port: UInt16, handle: @escaping (Request) -> Response) {
        self.port = port
        self.handle = handle
    }

    func start() {
        guard listening < 0 else { return }
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0, listen(fd, 8) == 0 else {
            NSLog("Desk Link: cannot listen on \(port): \(String(cString: strerror(errno)))")
            close(fd)
            return
        }
        listening = fd
        let thread = Thread { [weak self] in self?.accept(on: fd) }
        thread.name = "Desk Link server"
        thread.start()
    }

    func stop() {
        guard listening >= 0 else { return }
        let fd = listening
        listening = -1
        shutdown(fd, SHUT_RDWR)
        close(fd)
    }

    private func accept(on fd: Int32) {
        while true {
            var peer = sockaddr_in()
            var length = socklen_t(MemoryLayout<sockaddr_in>.size)
            let client = withUnsafeMutablePointer(to: &peer) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.accept(fd, $0, &length) }
            }
            guard client >= 0 else { return }  // closed by stop()
            let local = peer.sin_addr.s_addr == in_addr_t(0x7f00_0001).bigEndian
            queue.async { [weak self] in self?.serve(client, local: local) }
        }
    }

    private func serve(_ client: Int32, local: Bool) {
        defer { close(client) }
        var patience = Self.patience
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &patience, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &patience, socklen_t(MemoryLayout<timeval>.size))
        var yes: Int32 = 1
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, socklen_t(MemoryLayout<Int32>.size))
        var buffer = Data()
        var chunk = [UInt8](repeating: 0, count: 16 * 1024)
        while buffer.count <= Self.largest {
            let got = recv(client, &chunk, chunk.count, 0)
            guard got > 0 else { return }
            buffer.append(contentsOf: chunk[0..<got])
            if var request = Self.parse(buffer) {
                request.local = local
                write(handle(request), to: client)
                return
            }
        }
    }

    private func write(_ response: Response, to client: Int32) {
        var head = "HTTP/1.1 \(response.status) \(Self.reason(response.status))\r\n"
        head += "Content-Length: \(response.body.count)\r\nConnection: close\r\n"
        if !response.type.isEmpty { head += "Content-Type: \(response.type)\r\n" }
        head += "\r\n"
        let all = Data(head.utf8) + response.body
        all.withUnsafeBytes { raw in
            var sent = 0
            while sent < raw.count {
                let n = send(client, raw.baseAddress! + sent, raw.count - sent, 0)
                guard n > 0 else { return }
                sent += n
            }
        }
        shutdown(client, SHUT_WR)  // all said: the panel reads to the end, then closes
    }

    /// Nil until the whole request is in.
    private static func parse(_ data: Data) -> Request? {
        guard let end = data.range(of: Data("\r\n\r\n".utf8)) else { return nil }
        let lines = String(decoding: data[..<end.lowerBound], as: UTF8.self).components(separatedBy: "\r\n")
        let start = lines.first?.split(separator: " ") ?? []
        guard start.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[line[..<colon].lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let length = Int(headers["content-length"] ?? "") ?? 0
        let body = data[end.upperBound...]
        guard body.count >= length else { return nil }
        return Request(method: String(start[0]), path: String(start[1]), headers: headers,
                       body: Data(body.prefix(length)))
    }

    private static func reason(_ status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 204: return "No Content"
        case 400: return "Bad Request"
        case 403: return "Forbidden"
        default: return "Not Found"
        }
    }
}

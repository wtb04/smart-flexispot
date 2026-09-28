#include "notices.h"

#include "hass_protocol.h"
#include "ui.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <iterator>

namespace notices {
namespace {
constexpr int PORT = 47311;  // send.sh's, on localhost only

// One of each kind the card has: a title over a message, a warning that goes
// on its own, an error kept until tapped, and plain text.
constexpr const char *EXAMPLES[] = {
    R"({"title":"Washing machine","message":"The wash is done","level":"success"})",
    R"({"message":"The front door has been open for ten minutes","level":"warning","timeout_s":20})",
    R"({"title":"Backup","message":"Last night's backup did not finish","level":"error","timeout_s":0})",
    "Dinner is ready",
};

int s_next   = 0;
int s_socket = -1;
}  // namespace

void from_home_assistant(const std::string &payload)
{
    const hass::protocol::Notification notice = hass::protocol::parse_notification(payload);
    if (!notice.valid) {
        std::printf("W (sim) not a notice the panel would show: %s\n", payload.c_str());
        return;
    }
    const std::string &word  = notice.level;
    const ui::Level    level = word == "error"     ? ui::Level::Bad
                               : word == "warning" ? ui::Level::Warn
                               : word == "success" ? ui::Level::Good
                                                   : ui::Level::Neutral;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::notify("Home Assistant", notice.title.c_str(), notice.message.c_str(), level, notice.timeout_ms));
}

void next_example()
{
    from_home_assistant(EXAMPLES[s_next]);
    s_next = (s_next + 1) % static_cast<int>(std::size(EXAMPLES));
}

void listen()
{
    s_socket = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{};
    at.sin_family      = AF_INET;
    at.sin_port        = htons(PORT);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s_socket < 0 || bind(s_socket, reinterpret_cast<sockaddr *>(&at), sizeof(at)) != 0) {
        std::printf("W (sim) sim/send.sh cannot reach this one: port %d is taken\n", PORT);
        if (s_socket >= 0) {
            close(s_socket);
        }
        s_socket = -1;
        return;
    }
    fcntl(s_socket, F_SETFL, O_NONBLOCK);
}

void pump()
{
    if (s_socket < 0) {
        return;
    }
    char buffer[2048];
    for (;;) {
        const ssize_t got = recv(s_socket, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            return;
        }
        const std::string line(buffer, static_cast<std::size_t>(got));
        constexpr char    NOTIFY[] = "notify ";
        if (line.rfind(NOTIFY, 0) == 0) {
            from_home_assistant(line.substr(sizeof(NOTIFY) - 1));
        } else {
            std::printf("W (sim) send.sh said something unknown: %s\n", line.c_str());
        }
    }
}
}  // namespace notices

#include "travel_parse.h"

#include <cstring>

namespace travel {
namespace {

// Small enough to read by hand, and the shape is ours: the backend exists so
// that the panel never meets a journey planner's real JSON.
const char *skip_space(const char *at, const char *end)
{
    while (at < end && (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t')) {
        ++at;
    }
    return at;
}

// Between one element of an array and the next there is a comma as well.
const char *skip_gap(const char *at, const char *end)
{
    at = skip_space(at, end);
    while (at < end && *at == ',') {
        at = skip_space(at + 1, end);
    }
    return at;
}

// The next occurrence of "key" used as an object key, within one value's span.
const char *find_key(const char *at, const char *end, const char *key)
{
    const std::size_t len = std::strlen(key);
    for (; at + len + 2 < end; ++at) {
        if (*at != '"' || std::strncmp(at + 1, key, len) != 0 || at[len + 1] != '"') {
            continue;
        }
        const char *after = skip_space(at + len + 2, end);
        if (after < end && *after == ':') {
            return skip_space(after + 1, end);
        }
    }
    return nullptr;
}

bool read_number(const char *at, const char *end, std::int64_t &out)
{
    bool negative = false;
    if (at < end && *at == '-') {
        negative = true;
        ++at;
    }
    if (at >= end || *at < '0' || *at > '9') {
        return false;
    }
    std::int64_t value = 0;
    while (at < end && *at >= '0' && *at <= '9') {
        value = value * 10 + (*at++ - '0');
    }
    out = negative ? -value : value;
    return true;
}

void read_string(const char *at, const char *end, char *out, std::size_t size)
{
    out[0] = '\0';
    if (at >= end || *at != '"') {
        return;
    }
    ++at;
    std::size_t written = 0;
    while (at < end && *at != '"' && written + 1 < size) {
        if (*at == '\\' && at + 1 < end) {
            ++at;
        }
        out[written++] = *at++;
    }
    out[written] = '\0';
}

bool field_flag(const char *at, const char *end, const char *key)
{
    const char *found = find_key(at, end, key);
    return found != nullptr && end - found >= 4 && std::strncmp(found, "true", 4) == 0;
}

void field_number(const char *at, const char *end, const char *key, std::int64_t &out)
{
    const char *found = find_key(at, end, key);
    if (found != nullptr) {
        read_number(found, end, out);
    }
}

void field_string(const char *at, const char *end, const char *key, char *out, std::size_t size)
{
    const char *found = find_key(at, end, key);
    if (found != nullptr) {
        read_string(found, end, out, size);
    } else {
        out[0] = '\0';
    }
}

// The end of the bracketed value starting at `at`, honouring nesting and
// strings, so one option's fields are never read out of the next one.
const char *span_end(const char *at, const char *end)
{
    if (at >= end || (*at != '{' && *at != '[')) {
        return end;
    }
    const char open  = *at;
    const char close = open == '{' ? '}' : ']';
    int        depth = 0;
    bool       inside_string = false;

    for (; at < end; ++at) {
        if (inside_string) {
            if (*at == '\\') {
                ++at;
            } else if (*at == '"') {
                inside_string = false;
            }
            continue;
        }
        if (*at == '"') {
            inside_string = true;
        } else if (*at == open) {
            ++depth;
        } else if (*at == close && --depth == 0) {
            return at + 1;
        }
    }
    return end;
}

}  // namespace

int parse(const char *body, std::size_t length, Option *out, int capacity)
{
    if (body == nullptr || out == nullptr || capacity <= 0) {
        return 0;
    }
    const char *end     = body + length;
    const char *options = find_key(body, end, "options");
    if (options == nullptr || *options != '[') {
        return 0;
    }
    const char *options_end = span_end(options, end);

    int         stored = 0;
    const char *at     = options + 1;
    while (stored < capacity) {
        at = skip_gap(at, options_end);
        if (at >= options_end || *at != '{') {
            break;
        }
        const char *option_end = span_end(at, options_end);

        Option &option = out[stored];
        option         = Option{};
        field_number(at, option_end, "leaveAt", option.leave);
        field_number(at, option_end, "arriveAt", option.arrive);

        const char *legs = find_key(at, option_end, "legs");
        if (legs != nullptr && *legs == '[') {
            const char *legs_end = span_end(legs, option_end);
            const char *leg      = legs + 1;
            while (option.leg_count < kLegsMax) {
                leg = skip_gap(leg, legs_end);
                if (leg >= legs_end || *leg != '{') {
                    break;
                }
                const char *leg_end = span_end(leg, legs_end);
                Leg        &into    = option.legs[option.leg_count];

                field_string(leg, leg_end, "mode", into.mode, sizeof(into.mode));
                field_string(leg, leg_end, "line", into.line, sizeof(into.line));
                field_string(leg, leg_end, "from", into.from, sizeof(into.from));
                field_string(leg, leg_end, "to", into.to, sizeof(into.to));
                field_number(leg, leg_end, "dep", into.depart);
                field_number(leg, leg_end, "arr", into.arrive);
                into.cancelled = field_flag(leg, leg_end, "off");

                ++option.leg_count;
                leg = leg_end;
            }
        }
        for (int i = 0; i < option.leg_count; ++i) {
            option.cancelled = option.cancelled || option.legs[i].cancelled;
        }
        ++stored;
        at = option_end;
    }
    return stored;
}

}  // namespace travel

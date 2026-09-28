#include "last_words.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace last_words {
namespace {
constexpr char TAG[] = "restart";

constexpr std::uint32_t MAGIC       = 0x4c575264;  // "LWRd"
constexpr int           LINES       = 32;
constexpr std::size_t   LINE_BYTES  = 128;
constexpr std::size_t   REASON_SIZE = 48;
constexpr std::size_t   TASK_SIZE   = 16;
constexpr int           CALLERS     = 8;
constexpr int           STACK_WORDS = 96;

// Where this build's code is, from the linker, and where a stack can be.
extern "C" char _text_start[], _text_end[], _iram_text_start[], _iram_text_end[];
constexpr std::uint32_t RAM_FROM   = 0x4ff00000;
constexpr std::uint32_t RAM_TO     = 0x4ffc0000;
constexpr std::uint32_t PSRAM_FROM = 0x48000000;
constexpr std::uint32_t PSRAM_TO   = 0x4c000000;

bool code_at(std::uint32_t at)
{
    const auto in = [at](const char *from, const char *to) {
        return at >= reinterpret_cast<std::uintptr_t>(from) && at < reinterpret_cast<std::uintptr_t>(to);
    };
    return in(_text_start, _text_end) || in(_iram_text_start, _iram_text_end);
}

// Whole, so reading it cannot fault inside the panic handler.
bool stack_at(std::uint32_t sp, std::size_t bytes)
{
    const auto in = [&](std::uint32_t from, std::uint32_t to) { return sp >= from && sp + bytes <= to; };
    return (sp % sizeof(std::uint32_t)) == 0 && (in(RAM_FROM, RAM_TO) || in(PSRAM_FROM, PSRAM_TO));
}

struct Crash {
    std::uint32_t magic;
    int           core;
    std::uint32_t pc;
    std::uint32_t ra;
    std::uint32_t sp;
    std::uint32_t cause;
    std::uint32_t value;
    std::uint32_t callers[CALLERS];  // what on the stack looks like a return address
    int           caller_count;
    char          reason[REASON_SIZE];
    char          task[TASK_SIZE];
};

struct Kept {
    std::uint32_t magic;
    std::uint32_t next;
    char          lines[LINES][LINE_BYTES];
    Crash         crash;
};

// In the low-power RAM, which a restart leaves alone and which holds so little
// that it sits at the same place in every build: a build that crashes before
// it proves itself is rolled back, and the one before it still finds this.
RTC_NOINIT_ATTR Kept s_kept;

portMUX_TYPE   s_lock      = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_next_sink = nullptr;

int sink(const char *format, va_list args)
{
    if (!xPortInIsrContext()) {
        va_list copy;
        va_copy(copy, args);
        char line[LINE_BYTES];
        std::vsnprintf(line, sizeof(line), format, copy);
        va_end(copy);
        char *end = line + std::strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
        if (line[0] != '\0') {
            portENTER_CRITICAL(&s_lock);
            std::memcpy(s_kept.lines[s_kept.next % LINES], line, sizeof(line));
            s_kept.next = (s_kept.next + 1) % LINES;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    return s_next_sink != nullptr ? s_next_sink(format, args) : 0;
}

const char *reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_SW: return "asked to";
        case ESP_RST_PANIC: return "a crash";
        case ESP_RST_INT_WDT: return "the interrupt watchdog";
        case ESP_RST_TASK_WDT: return "the task watchdog";
        case ESP_RST_WDT: return "a watchdog";
        case ESP_RST_BROWNOUT: return "the supply dipping";
        case ESP_RST_DEEPSLEEP: return "waking";
        default: return "an unknown reason";
    }
}

void tell_crash(const Crash &crash)
{
    ESP_LOGW(TAG, "crash on core %d in %s: %s", crash.core, crash.task, crash.reason);
    ESP_LOGW(TAG, "pc 0x%08lx ra 0x%08lx sp 0x%08lx cause %lu value 0x%08lx", static_cast<unsigned long>(crash.pc),
             static_cast<unsigned long>(crash.ra), static_cast<unsigned long>(crash.sp),
             static_cast<unsigned long>(crash.cause), static_cast<unsigned long>(crash.value));
    char line[LINE_BYTES] = "";
    int  n                = 0;
    for (int i = 0; i < crash.caller_count && i < CALLERS; ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, " 0x%08lx", static_cast<unsigned long>(crash.callers[i]));
    }
    ESP_LOGW(TAG, "stack:%s", line);
}
}  // namespace

void start()
{
    const esp_reset_reason_t reason   = esp_reset_reason();
    const bool               unasked  = reason != ESP_RST_POWERON && reason != ESP_RST_SW &&
                                        reason != ESP_RST_DEEPSLEEP;
    const bool               had_kept = s_kept.magic == MAGIC;
    if (unasked) {
        ESP_LOGW(TAG, "restarted after %s", reason_name(reason));
    } else {
        ESP_LOGI(TAG, "started after %s", reason_name(reason));
    }
    if (unasked && had_kept) {
        if (s_kept.crash.magic == MAGIC) {
            tell_crash(s_kept.crash);
        }
        for (int i = 0; i < LINES; ++i) {
            const char *line = s_kept.lines[(s_kept.next + i) % LINES];
            if (line[0] != '\0' && std::memchr(line, '\0', LINE_BYTES) != nullptr) {
                ESP_LOGW(TAG, "before: %s", line);
            }
        }
    }
    std::memset(&s_kept, 0, sizeof(s_kept));
    s_kept.magic = MAGIC;
    s_next_sink  = esp_log_set_vprintf(sink);
}
}  // namespace last_words

// Linked in place of the panic handler, which is left to do everything it
// did: this only notes where first, in the memory the restart keeps.
extern "C" void __real_esp_panic_handler(panic_info_t *info);

extern "C" void __wrap_esp_panic_handler(panic_info_t *info)
{
    using namespace last_words;
    Crash &crash = s_kept.crash;
    std::memset(&crash, 0, sizeof(crash));
    crash.core = info->core;
    std::snprintf(crash.reason, sizeof(crash.reason), "%s", info->reason != nullptr ? info->reason : "");
    const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(info->core);
    std::snprintf(crash.task, sizeof(crash.task), "%s", task != nullptr ? pcTaskGetName(task) : "?");
    if (const auto *frame = static_cast<const RvExcFrame *>(info->frame); frame != nullptr) {
        crash.pc    = frame->mepc;
        crash.ra    = frame->ra;
        crash.sp    = frame->sp;
        crash.cause = frame->mcause;
        crash.value = frame->mtval;
        if (stack_at(frame->sp, STACK_WORDS * sizeof(std::uint32_t))) {
            const auto *stack = reinterpret_cast<const std::uint32_t *>(frame->sp);
            for (int i = 0; i < STACK_WORDS && crash.caller_count < CALLERS; ++i) {
                if (code_at(stack[i])) {
                    crash.callers[crash.caller_count++] = stack[i];
                }
            }
        }
    }
    crash.magic = MAGIC;
    __real_esp_panic_handler(info);
}

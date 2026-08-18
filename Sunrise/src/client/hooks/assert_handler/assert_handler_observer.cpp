#include "assert_handler_observer.h"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../targets/game/assert_handler.h"
#include "../retail_log/retail_log_enqueue_observer.h"

namespace sunrise::client::hooks::assert_handler {
namespace {

/** The game formats assert text into a buffer of this size, so it bounds ours too. */
constexpr std::size_t kTextCapacity = 1024;
/** One log line carries the message plus its fixed prefix. */
constexpr std::size_t kLineCapacity = 1152;
/** Consecutive repeats of one message written in full before counting takes over. */
constexpr std::uint32_t kRepeatHead = 8;
/** One repeat in this many is written after that, so a stuck assert never goes silent. */
constexpr std::uint32_t kRepeatStride = 512;
/** Used when the game's own format string cannot be printed. */
constexpr char kUnformattable[] = "<unformattable>";
/** Watchdog message whose first occurrence receives the lock-free retail progress snapshot. */
constexpr std::string_view kNetworkHitchPrefix = "hitch detected: mainloop world controller";
/** One progress line carries two sites, two serials, and the active native-call count. */
constexpr std::size_t kProgressLineCapacity = 320;

/**
 * Halt category of the graphics device-loss assert. A removed device never returns, so this one
 * must keep halting the process. Every other category stays non-fatal.
 */
constexpr int kGraphicsHaltCategory = 6;

/** The handler the game installed, called with the same printf-style arguments the sites use. */
using NativeHandler = void(__cdecl*)(int, const char*, ...);

SRWLOCK g_lock{SRWLOCK_INIT};
/** Last message seen, so a message that repeats every frame is counted rather than written. */
std::array<char, kTextCapacity> g_lastText{};
std::uint32_t g_repeats{};
std::uint32_t g_seen{};

/** @param code Halt category from the assert site. @return True when the process must still die. */
[[nodiscard]] bool unrecoverable(int code) noexcept {
    return code == kGraphicsHaltCategory;
}

/**
 * Counts one message and says whether this occurrence is written.
 * @param text Formatted assert message.
 * @param seen Receives the total assert count.
 * @param repeats Receives the length of the current run of this message.
 * @return True when the caller writes a log line.
 */
[[nodiscard]] bool admit(const char* text, std::uint32_t& seen, std::uint32_t& repeats) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    ++g_seen;
    if (std::strcmp(g_lastText.data(), text) == 0) {
        ++g_repeats;
    } else {
        const std::size_t length = std::strlen(text);
        const std::size_t copied = length < kTextCapacity ? length : kTextCapacity - 1;
        std::memcpy(g_lastText.data(), text, copied);
        g_lastText[copied] = '\0';
        g_repeats = 1;
    }
    seen = g_seen;
    repeats = g_repeats;
    ReleaseSRWLockExclusive(&g_lock);
    return repeats <= kRepeatHead || repeats % kRepeatStride == 0;
}

/**
 * Records one assert without letting logging failures reach the game.
 * @param code Halt category the assert site passes. Zero, one and six all occur.
 * @param text Already-formatted assert message.
 */
void report(int code, const char* text) noexcept {
    std::uint32_t seen = 0;
    std::uint32_t repeats = 0;
    if (!admit(text, seen, repeats)) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=assert stage=hit n=%u repeat=%u arg0=%d text=%s",
                                      seen,
                                      repeats,
                                      code,
                                      text);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::error, {line.data(), length});
    if (repeats == 1 && std::string_view(text).starts_with(kNetworkHitchPrefix)) {
        const retail_log::ProgressSnapshot progress = retail_log::progress_snapshot();
        std::array<char, kProgressLineCapacity> progressLine{};
        const int progressWritten = std::snprintf(
            progressLine.data(),
            progressLine.size(),
            "ev=assert stage=network-hitch retail_enter=%d/%llu/+0x%llX/%lu "
            "retail_return=%d/%llu/+0x%llX/%lu observer=%u native=%u",
            progress.enteredSite,
            static_cast<unsigned long long>(progress.enteredSerial),
            static_cast<unsigned long long>(progress.enteredCallerRva),
            static_cast<unsigned long>(progress.enteredThread),
            progress.returnedSite,
            static_cast<unsigned long long>(progress.returnedSerial),
            static_cast<unsigned long long>(progress.returnedCallerRva),
            static_cast<unsigned long>(progress.returnedThread),
            progress.activeObserverCalls,
            progress.activeNativeCalls);
        if (progressWritten > 0) {
            const auto progressLength =
                static_cast<std::size_t>(progressWritten) < progressLine.size()
                    ? static_cast<std::size_t>(progressWritten)
                    : progressLine.size() - 1;
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             {progressLine.data(), progressLength});
        }
    }
}

/**
 * Hands one assert back to the handler the game installed, which halts the process.
 * @param code Halt category from the assert site.
 * @param text Message this handler already formatted.
 */
void chain(int code, const char* text) noexcept {
    if (!targets::game::assert_handler::is_resolved()) {
        return;
    }
    const targets::game::assert_handler::Targets& resolved = targets::game::assert_handler::get();
    if (resolved.original == nullptr) {
        return;
    }
    std::array<char, 64> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=assert stage=halt arg0=%d result=native", code);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    // The original is printf-style too, so the text already built is passed as its one argument.
    reinterpret_cast<NativeHandler>(resolved.original)(code, "%s", text);
}

/**
 * Replacement assert handler. The sites call this slot as a printf-style callback. Returning
 * without calling the game's own handler is what makes the assert non-fatal. That handler builds
 * a crash ticket, shows a dialog and blocks.
 * @param code Halt category from the assert site.
 * @param format Native printf-style format string.
 */
void __cdecl handler_body(int code, const char* format, ...) noexcept {
    std::array<char, kTextCapacity> text{};
    if (format == nullptr) {
        std::memcpy(text.data(), kUnformattable, sizeof kUnformattable);
    } else {
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(text.data(), text.size(), format, arguments);
        va_end(arguments);
        if (written < 0) {
            std::memcpy(text.data(), kUnformattable, sizeof kUnformattable);
        }
    }
    text.back() = '\0';

    // An assert raised by our own logging must not re-enter this body.
    static thread_local bool inside = false;
    if (inside) {
        return;
    }
    inside = true;
    report(code, text.data());
    // Swallowing an unrecoverable halt leaves the process running with nothing to present. The
    // original handler runs so the process dies the way the game intends.
    if (unrecoverable(code)) {
        chain(code, text.data());
    }
    inside = false;
}

} // namespace

/** @return Address of the internal assert handler body. */
void* handler_entry_point() noexcept {
    return reinterpret_cast<void*>(&handler_body);
}

} // namespace sunrise::client::hooks::assert_handler

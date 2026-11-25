#pragma once

// Typed, opt-in return observation. No generic six-argument function casts:
// the thunk has the exact target signature, including stack arguments and FP.
// The host test uses the Microsoft x64 ABI too, without needing Wine.
#include <atomic>
#include <cstdint>
#include <tuple>
#include <type_traits>

#ifdef _WIN32
#define XSX_RETURN_ABI __stdcall
#else
#define XSX_RETURN_ABI __attribute__((ms_abi))
#endif

namespace xsx::returns {
enum class Contract { HResult, NonNull, ReferenceCount, Value };
enum class Finding { None, FailedHResult, NullOutput, UnreadableOutput, NullReturn };
struct Site {
    void* target = nullptr;
    const char* method = nullptr;
    int kind = 0, slot = 0;
    Contract contract = Contract::HResult;
};
struct Event {
    Site site;
    uint64_t call = 0, returned = 0, failure = 0;
    uintptr_t caller = 0, object = 0, value = 0, output = 0, output_value = 0;
    bool output_readable = false;
    Finding finding = Finding::None;
};
struct Hooks {
    int (*mode)() = nullptr; // 0: off, 1: failures only, 2: every typed return
    uint32_t (*get_error)() = nullptr;
    void (*set_error)(uint32_t) = nullptr;
    bool (*read_word)(uintptr_t, uintptr_t&) = nullptr;
    void (*emit)(const Event&) = nullptr;
};
inline Hooks hooks;
inline std::atomic<uint64_t> calls{0}, returned{0}, failures{0}, installed{0};

inline int Mode() { return hooks.mode ? hooks.mode() : 0; }
inline const char* FindingName(Finding finding)
{
    switch (finding) {
    case Finding::FailedHResult: return "HRESULT_FAILURE";
    case Finding::NullOutput: return "NULL_OUTPUT";
    case Finding::UnreadableOutput: return "UNREADABLE_OUTPUT";
    case Finding::NullReturn: return "NULL_RETURN";
    default: return "RETURN";
    }
}

template<class T> static uintptr_t Bits(T value)
{
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uintptr_t>(value);
}

// Only the signature's last pointer-to-pointer is a COM output. Do not guess
// that an arbitrary non-null argument points to an output structure.
template<class T> static uintptr_t OutputAddress(T value)
{
    if constexpr (std::is_pointer_v<T> &&
                  std::is_pointer_v<std::remove_pointer_t<T>>) return Bits(value);
    else return 0;
}

template<int Kind, int Slot, class Fn> struct Thunk;
template<int Kind, int Slot, class R, class... Args>
struct Thunk<Kind, Slot, R (XSX_RETURN_ABI*)(Args...)> {
    using Fn = R (XSX_RETURN_ABI*)(Args...);
    inline static Site site;

    __attribute__((noinline)) static R XSX_RETURN_ABI Call(Args... args)
    {
        const int mode = Mode();
        const auto fn = reinterpret_cast<Fn>(site.target);
        if (!mode) return fn(args...);
        Event event;
        event.site = site;
        event.call = ++calls;
        event.caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        auto tuple = std::forward_as_tuple(args...);
        if constexpr (sizeof...(Args) != 0) event.object = Bits(std::get<0>(tuple));
        if constexpr (sizeof...(Args) > 1)
            event.output = OutputAddress(std::get<sizeof...(Args) - 1>(tuple));

        R result = fn(args...); // Keep the compiler's exact argument/return ABI.
        const uint32_t error = hooks.get_error ? hooks.get_error() : 0;
        event.value = Bits(result);
        event.returned = ++returned;
        if (site.contract == Contract::HResult) {
            const int32_t hr = static_cast<int32_t>(event.value);
            if (hr < 0) event.finding = Finding::FailedHResult;
            // Null ppv is valid for validation-only calls (usually S_FALSE).
            // Only S_OK with a supplied output has a non-null-object contract.
            if (hr == 0 && event.output) {
                event.output_readable = hooks.read_word &&
                    hooks.read_word(event.output, event.output_value);
                if (!event.output_readable) event.finding = Finding::UnreadableOutput;
                else if (!event.output_value) event.finding = Finding::NullOutput;
            }
        } else if (site.contract == Contract::NonNull && !event.value) {
            event.finding = Finding::NullReturn;
        }
        if (event.finding != Finding::None) event.failure = ++failures;
        if ((event.failure || mode >= 2) && hooks.emit) hooks.emit(event);
        if (hooks.set_error) hooks.set_error(error);
        return result;
    }
};

// The signature argument supplies a type only; the target is the FINAL entry
// already installed in our wrapper table. Thus recovered internal fallbacks
// are not incorrectly reported as failures returned to the guest.
template<int Kind, int Slot, class Fn>
static void Install(void** table, Fn, const char* name,
                    Contract contract = Contract::HResult)
{
    using T = Thunk<Kind, Slot, Fn>;
    if (!Mode() || table[Slot] == reinterpret_cast<void*>(&T::Call)) return;
    T::site = {table[Slot], name, Kind, Slot, contract};
    table[Slot] = reinterpret_cast<void*>(&T::Call);
    ++installed;
}

template<class Member, class Self> struct MemberSignature;
template<class R, class Class, class... Args, class Self>
struct MemberSignature<R (XSX_RETURN_ABI Class::*)(Args...), Self> {
    using Fn = R (XSX_RETURN_ABI*)(Self*, Args...);
};
template<int Kind, int Slot, class Self, class Member>
static void InstallMember(void** table, Member, const char* name,
                          Contract contract = Contract::HResult)
{
    using Fn = typename MemberSignature<Member, Self>::Fn;
    Install<Kind, Slot>(table, Fn{}, name, contract);
}
} // namespace xsx::returns

/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_GPIO_H
#define AIRGRADIENT_GPIO_H

// GPIO backends expose a shared operations table and a borrowed instance context.
// Native GPIO uses a null context; stateful backends borrow caller-owned state.

namespace gpio {

inline constexpr int INVALID_PIN = -1;
inline constexpr int KEEP_OUTPUT_LEVEL = -1;

// Interrupt service routine callback. Called from ISR context when a configured interrupt fires.
// The handler must follow ISR-safe rules: no blocking, no heap allocation, no logging.
using InterruptHandler = void (*)(void *arg);

enum class Mode {
  Input,
  Output,
  InputOutput,
};

enum class PullMode {
  Floating,
  PullUp,
  PullDown,
  PullUpDown,
};

enum class InterruptType {
  Disabled,
  RisingEdge,
  FallingEdge,
  AnyEdge,
  LowLevel,
  HighLevel,
};

// Operations are called from task context and may block. Unsupported operations
// may be null; Hal returns failure for those operations. get_level returns -1 on
// failure. Thread safety and allocation depend on the backend.
struct Operations {
  bool (*configure)(void *context, int pin, Mode mode, PullMode pull, InterruptType interrupt,
                    int initial_level) = nullptr;
  int (*get_level)(void *context, int pin) = nullptr;
  bool (*set_level)(void *context, int pin, int level) = nullptr;
  bool (*add_interrupt_handler)(void *context, int pin, InterruptHandler handler,
                                void *arg) = nullptr;
  bool (*remove_interrupt_handler)(void *context, int pin) = nullptr;
  bool (*enable_interrupt)(void *context, int pin) = nullptr;
  bool (*disable_interrupt)(void *context, int pin) = nullptr;
};

// Copyable, non-owning backend binding. Both the operations table and context
// must outlive all copies. The context must remain at a stable
// address. A null context is valid for stateless backends.
struct Hal {
  const Operations &operations;
  void *context;

  constexpr Hal(const Operations &operations, void *context = nullptr)
      : operations(operations), context(context) {}
  Hal(Operations &&, void * = nullptr) = delete;
  Hal(const Operations &&, void * = nullptr) = delete;

  // initial_level applies to output modes: 0/1 loads the latch before enabling
  // the output driver; KEEP_OUTPUT_LEVEL preserves it. Four-argument calls keep
  // the existing latch, matching the original native GPIO behavior.
  bool configure(int pin, Mode mode, PullMode pull, InterruptType interrupt,
                 int initial_level = KEEP_OUTPUT_LEVEL) const {
    return operations.configure != nullptr &&
           operations.configure(context, pin, mode, pull, interrupt, initial_level);
  }
  int get_level(int pin) const {
    return operations.get_level != nullptr ? operations.get_level(context, pin) : -1;
  }
  bool set_level(int pin, int level) const {
    return operations.set_level != nullptr && operations.set_level(context, pin, level);
  }
  // Callback and arg must remain valid until the handler is removed.
  bool add_interrupt_handler(int pin, InterruptHandler handler, void *arg) const {
    return handler != nullptr && operations.add_interrupt_handler != nullptr &&
           operations.add_interrupt_handler(context, pin, handler, arg);
  }
  bool remove_interrupt_handler(int pin) const {
    return operations.remove_interrupt_handler != nullptr &&
           operations.remove_interrupt_handler(context, pin);
  }
  bool enable_interrupt(int pin) const {
    return operations.enable_interrupt != nullptr && operations.enable_interrupt(context, pin);
  }
  bool disable_interrupt(int pin) const {
    return operations.disable_interrupt != nullptr && operations.disable_interrupt(context, pin);
  }
};

// Copies the backend binding, not its hardware. Safe to construct from a
// backend's hal() return value; the operations table and context still outlive it.
struct Pin {
  Hal hal;
  int number = INVALID_PIN;

  bool configure(Mode mode, PullMode pull, InterruptType interrupt,
                 int initial_level = KEEP_OUTPUT_LEVEL) const {
    return hal.configure(number, mode, pull, interrupt, initial_level);
  }
  int get_level() const { return hal.get_level(number); }
  bool set_level(bool high) const { return hal.set_level(number, high ? 1 : 0); }
};

} // namespace gpio

#endif // AIRGRADIENT_GPIO_H

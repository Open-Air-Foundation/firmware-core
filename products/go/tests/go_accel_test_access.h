#pragma once

#include "accel/accel_service.h"

// Drive the actual worker setup/iteration without creating a host RTOS thread.
class AccelServiceTestAccess {
public:
  static bool initialize(AccelService &service) {
    service._work_queue =
        RTOS::queue_create(AccelService::WORK_QUEUE_DEPTH, sizeof(AccelService::Command));
    service._reply_queue = RTOS::queue_create(1, sizeof(AccelService::Reply));
    service._running.store(true);
    return service.initialize_capture();
  }
  static bool poll(AccelService &service, bool notified = false) {
    if (notified) {
      service.process_command(AccelService::Command::Interrupt);
    }
    return service.poll_capture();
  }
  static bool is_reply_queue(const AccelService &service, RtosQueueHandle queue) {
    return queue == service._reply_queue;
  }
  // Cooperative host scheduling: execute the real worker command handler when
  // the orchestrator waits for its reply, with no production-only shortcut.
  static void process_pending(AccelService &service) {
    AccelService::Command command;
    while (service._running.load() && RTOS::queue_receive(service._work_queue, &command, 0)) {
      service.process_command(command);
    }
  }
  static bool queue_stop(AccelService &service) {
    const auto command = AccelService::Command::Stop;
    return RTOS::queue_send(service._work_queue, &command, 0);
  }
  static bool queue_interrupt(AccelService &service) {
    // ISR queue sends are no-ops in the host RTOS implementation.
    const auto command = AccelService::Command::Interrupt;
    return RTOS::queue_send(service._work_queue, &command, 0);
  }
  static bool running(const AccelService &service) { return service._running.load(); }
  static RtosQueueHandle work_queue(const AccelService &service) { return service._work_queue; }
  static bool burst(const AccelService &service) { return service._burst_active; }
};

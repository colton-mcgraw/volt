# Bug Tracker

## Open

- Rendering regression after event-driven wake changes:
  - Symptom: frame output can appear stalled or not visible after switching wake logic.
  - Current mitigation: main loop uses dynamic timeout (`kLogicTickSeconds` foreground, `kMinimizedWakeSeconds` minimized) instead of fixed 1 second waits.
  - Follow-up: verify redraw behavior across resize/minimize/restore and when idle with no input.

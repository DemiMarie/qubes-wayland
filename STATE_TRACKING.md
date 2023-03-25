# State Tracking in the Wayland Agent

The Wayland agent must maintain a lot of per-window state.
Doing so improperly has been a significant source of bugs.
This document specifies how the state should be handled.

## Window size and position

`struct qubes_output` stores two copies of the size and position of each window.

- `struct qubes_output::host` stores the most recent values sent by the host, or the guest’s values if no `MSG_CONFIGURE` has been received yet.
- `struct qubes_output::guest` stores the most recent values known, period.
  A valid `MSG_CONFIGURE` from the host updates both `qubes_output::host` and `qubes_output::guest`.

Additionally, if `struct qubes_output::flags` contains `QUBES_OUTPUT_IGNORE_CLIENT_RESIZE`, changes to the bounding box provided by the client must be ignored.

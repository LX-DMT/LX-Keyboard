# Keyboard lighting (product USB only)

Message 20 is the request; message 21 is the reply. Version is 1.
The reply repeats the operation and has a little-endian 16-bit ESP error code.

| Operation | Request bytes | Reply bytes |
|---|---|---|
| Read low-voltage policy | `1, 0` | `1, 0, rc_lo, rc_hi, enabled` |
| Save low-voltage policy | `1, 1, enabled` | `1, 1, rc_lo, rc_hi, enabled` |
| Read effect | `1, 2` | `1, 2, rc_lo, rc_hi, mode, h, s, v, speed` |
| Save effect | `1, 3, mode, h, s, v, speed` | `1, 3, rc_lo, rc_hi, mode, h, s, v, speed` |

`enabled` is 0 or 1 (factory default 1). HSV channels range from 0 to 255;
brightness keeps the original hardware output limit. `speed` ranges from 1
to 100 (default 60), with period `6000 - speed * 50` milliseconds.
It applies to breathing, blinking and hue cycling. Modes: 0 steady,
1 breathing, 2 audio envelope, 3 blink, 4 hue cycling, 5 decorative lights off.
Legacy hue-cycle settings remain readable. Four battery pixels keep
their independent display. Sleep, shutdown, low-voltage policy and activation
overlays retain priority over decorative effects.

Only a zero error code confirms success; values in an error reply must not
be applied by the host. Successful writes commit NVS before acknowledging;
the host reads back before displaying success. No write occurs for unchanged
values. Pairing, network, audio switch and custom keys are not modified.

The PC effect menu exposes IDs 0, 1, 2 and 5 only. IDs 3 and 4 remain
read-compatible so removing menu items never changes protocol numbering.
ID 2 is shared with the physical knob. Its microphone level uses
an independent 180 ms decaying envelope across capture leases, cleared when
audio hardware stops; it does not alter captured PCM or wake-word processing.

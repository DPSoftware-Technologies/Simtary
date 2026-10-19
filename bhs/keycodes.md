# BHS key codes

The `code` field of key events (PROTOCOL.md 2.1) is the **USB HID usage id** of the key (page 0x07,
"Keyboard/Keypad"), which is also what SDL calls a scancode. It names the physical key, not the
character it types, exactly like the W3C `KeyboardEvent.code` the browser reports.

| W3C `code` | HID |
|---|---|
| `KeyA` .. `KeyZ` | 4 .. 29 |
| `Digit1` .. `Digit9`, `Digit0` | 30 .. 38, 39 |
| `Enter`, `Escape`, `Backspace`, `Tab`, `Space` | 40, 41, 42, 43, 44 |
| `F1` .. `F12` | 58 .. 69 |
| `ArrowRight`, `ArrowLeft`, `ArrowDown`, `ArrowUp` | 79, 80, 81, 82 |
| `ControlLeft`, `ShiftLeft`, `AltLeft` | 224, 225, 226 |
| `ControlRight`, `ShiftRight`, `AltRight` | 228, 229, 230 |

The reference viewer (`web/index.html`) holds this table in JavaScript; the engine side
(`Framework/stream/StreamHost.cpp`, `ButtonFromHid`) maps the same numbers to its own buttons. Keys that
are not in the table are not sent yet; extending it means adding the W3C name here and in both places.

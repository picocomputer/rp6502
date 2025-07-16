# Xbox One XInput Protocol Support

This implementation adds Xbox One XInput support to the RP6502 gamepad system, based on the excellent work from the [GP2040-CE project](https://github.com/OpenStickCommunity/GP2040-CE).

## What Was Added

### 1. Xbox One Controller Detection (`des.c`)

Added detection for Xbox One controllers using vendor/product ID matching:

- **Microsoft Controllers (0x045E)**:
  - Xbox One Controller (0x02D1)
  - Xbox One Controller Firmware 2015 (0x02DD)
  - Xbox One Elite Controller (0x02E3)
  - Xbox One S Controller (0x02EA, 0x02FD)
  - Xbox One Elite Series 2 (0x0B00, 0x0B05)
  - Xbox Series X/S Controller (0x0B12, 0x0B13, 0x0B20, 0x0B21)

- **Third-Party Controllers**:
  - PDP (Performance Designed Products) - 0x0E6F
  - PowerA - 0x24C6
  - Hori - 0x0F0D
  - Razer - 0x1532

### 2. Xbox One Report Structure

Xbox One controllers use a unique report structure based on the GameInput Protocol (GIP):

```c
typedef struct {
    // GIP Header (4 bytes)
    uint8_t  command;      // 0x20 for input reports
    uint8_t  unknown;
    uint8_t  sequence;
    uint8_t  length;

    // Button data
    uint8_t  dpad_buttons; // Individual D-pad bits (not hat)
    uint8_t  menu_guide;   // Menu/Guide buttons
    uint8_t  face_buttons; // A, B, X, Y + shoulder buttons
    uint8_t  stick_buttons; // L3, R3 clicks

    // Analog data (all 16-bit values)
    uint16_t left_trigger;  // 10-bit precision
    uint16_t right_trigger; // 10-bit precision
    int16_t  left_stick_x;  // Signed 16-bit
    int16_t  left_stick_y;  // Signed 16-bit
    int16_t  right_stick_x; // Signed 16-bit
    int16_t  right_stick_y; // Signed 16-bit

    // Reserved padding
    uint8_t  reserved[18];
} xbox_one_report_t;
```

### 3. Key Differences from Standard HID

- **No Report ID**: Xbox One input reports don't use a report ID byte
- **16-bit Analog Values**: Sticks and triggers use 16-bit precision vs 8-bit HID
- **Individual D-pad Buttons**: D-pad sent as separate button bits, not hat switch
- **GIP Protocol**: Uses Xbox's GameInput Protocol with specific packet structure

### 4. Button Mapping

The implementation maps Xbox One buttons to the standard gamepad format:

| Xbox One | GP2040 | Standard |
|----------|--------|----------|
| A        | B1     | Button 1 |
| B        | B2     | Button 2 |
| X        | B3     | Button 3 |
| Y        | B4     | Button 4 |
| LB       | L1     | Left Shoulder |
| RB       | R1     | Right Shoulder |
| LT       | rx     | Left Trigger (analog) |
| RT       | ry     | Right Trigger (analog) |
| View     | S1     | Select/Back |
| Menu     | S2     | Start |
| LS       | L3     | Left Stick Click |
| RS       | R3     | Right Stick Click |

### 5. Analog Scaling

Xbox One controllers provide higher precision than standard HID:

- **Analog Sticks**: 16-bit signed values (-32768 to +32767) scaled to 8-bit (0-255)
- **Triggers**: 16-bit values (10-bit actual precision) scaled to 8-bit (0-255)

The conversion preserves the full range while maintaining compatibility with the existing gamepad interface.

### 6. D-pad Handling

Xbox One D-pad is sent as individual button bits rather than a hat switch:

```c
// Xbox One D-pad bits in byte 1
bool up    = (dpad_byte & (1 << 0)) != 0;
bool down  = (dpad_byte & (1 << 1)) != 0;
bool left  = (dpad_byte & (1 << 2)) != 0;
bool right = (dpad_byte & (1 << 3)) != 0;

// Convert to standard hat values (0-7 clockwise, 8=center)
```

## Files Modified

1. **`src/ria/usb/des.c`**:
   - Added `xbox_one_descriptor` with proper bit mappings
   - Added `des_xbox_one_controller()` function for VID/PID detection
   - Updated `des_report_descriptor()` to check Xbox One controllers first

2. **`src/ria/usb/pad.c`**:
   - Added Xbox One detection in `pad_parse_report_to_gamepad()`
   - Added special handling for 16-bit analog stick scaling
   - Added special handling for 16-bit trigger scaling
   - Added D-pad button-to-hat conversion
   - Fixed button offset comparison (0xFFFF vs 0xFF)

3. **`src/ria/usb/pad.h`**:
   - Updated header documentation to mention Xbox One support

## Testing

The implementation should be tested with:

1. **Microsoft Xbox One/Series controllers** (wired and wireless via adapter)
2. **Third-party Xbox One compatible controllers**
3. **Existing controllers** to ensure no regression

## Future Enhancements

1. **Guide Button Support**: Xbox One guide button uses virtual keycodes - could be added
2. **Haptic Feedback**: Xbox One supports rumble - could be implemented
3. **Adaptive Triggers**: Xbox Series controllers support advanced haptics
4. **Battery Status**: Wireless controllers report battery level

## References

- [GP2040-CE Xbox One Implementation](https://github.com/OpenStickCommunity/GP2040-CE/tree/main/src/drivers/xbone)
- [Xbox One GameInput Protocol Documentation](https://github.com/OpenStickCommunity/GP2040-CE/blob/main/headers/drivers/xbone/XBOneDescriptors.h)
- [Microsoft XInput Documentation](https://docs.microsoft.com/en-us/windows/win32/xinput/xinput-game-controller-apis-portal)

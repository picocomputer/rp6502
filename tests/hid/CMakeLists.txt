# Pointers and pads, each driven by the SDK demo that exercises it.

# --- 6522 VIA timer IRQ + HID mouse, via the paint_mou demo ---
rp6502_add_test(paint_mou LIBS emu_core FIXTURE paint_mou.rp6502 TIMEOUT 60)

# --- Absolute pointer (tablet) window encoding, via paint_tab ---
rp6502_add_test(paint_tab LIBS emu_core FIXTURE paint_tab.rp6502 TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror), via the gamepad tester ---
rp6502_add_test(gamepad LIBS emu_core FIXTURE gamepad.rp6502 TIMEOUT 60)

# TinyUSB host overrides

These are modified copies of TinyUSB host files. The RIA builds them instead of the upstream files. They will not go upstream, so code comments stay short and this file holds the reasons.

Upstream base: `vendor/tinyusb` at master `8692556e335a0bf3b0a054a959efd2a3ee677cb6`.

## Build

- `src/host/pico/ria.cmake` marks the six upstream sources `HEADER_FILE_ONLY`, so they are not compiled. It then adds these copies to the RIA target (`rp6502-ria` or `rp6502-ria-w`) with `target_sources`.
- CMake source file properties apply per directory. The VGA builds in `src/host/pico/vga`, so it compiles upstream TinyUSB unmodified, including upstream `rp2040_usb.c` for its device stack.
- The top-level `CMakeLists.txt` sets `PICO_TINYUSB_PATH` to `vendor/tinyusb`, so the SDK uses the submodule instead of its bundled TinyUSB.
- No headers are copied. The copies include upstream headers by their path under TinyUSB's `src/`, for example `host/usbh_pvt.h` and `portable/raspberrypi/rp2040/rp2040_usb.h`. A new function shared between files is declared in the file that calls it:
  - `hcd_free_ep_count()` (hcd_rp2040.c) is declared in `src/host/pico/ria/usb/usb.c` and feeds the "ep free" status line.
  - `usbh_driver_set_config_failed()` (usbh.c) is declared in hub.c.
  - `tuh_midi_frame()` (midi_host.c) is declared in `src/host/pico/ria/usb/mid.c`.
- Every copy adds a `Copyright (c) 2026 Rumbledethumps` line. midi_host.c carries the full MIT text and the others use SPDX lines. No copy is reformatted, so `diff -u` against upstream shows only real changes.

| Copy | Upstream, under `vendor/tinyusb/src/` | Summary |
|---|---|---|
| hcd_rp2040.c | portable/raspberrypi/rp2040/hcd_rp2040.c | Host controller driver. Every transfer runs on EPX, scheduled in software. About half the file is rewritten. |
| rp2040_usb.c | portable/raspberrypi/rp2040/rp2040_usb.c | 12-cycle wait in host mode. Host EP0 is single-buffered. |
| usbh.c | host/usbh.c | A failed enumeration ends and cleans up instead of stranding. |
| hub.c | host/hub.c | Fixes set-config strands, reads the multi-byte status bitmap, sends the right descriptor wValue, adds an address guard. |
| cdc_host.c | class/cdc/cdc_host.c | Fixes the CH34X chip version strand. |
| midi_host.c | class/midi/midi_host.c | MIDI host rework, plus the `tuh_midi_frame()` wire framer. |

**Strand:** an enumeration failure that returns without ending the enumeration. usbh keeps `enumerating_daddr` set, so every later attach waits. A strand lasts until usbh sees the device, or its hub, unplugged. usbh re-arms a hub's status endpoint only at `ENUM_CONFIG_DRIVER`. So if the device sits behind a hub and fails before that step, the device's own unplug goes unseen, and only unplugging the hub ends the strand.

## hcd_rp2040.c

The RP2350 host controller has one endpoint driven by software (EPX) and 15 hardware interrupt-endpoint slots that poll on their own. Upstream runs control and bulk transfers on EPX and interrupt endpoints on the slots. This copy runs everything on EPX.

The file stops the build with `#error` on anything but the RP2350. The scheduler needs `STOP_EPX_ON_NAK` and `EPX_STOPPED_ON_NAK`, and only the RP2350 has them.

### EPX only

- Control, bulk and interrupt transfers all run on EPX. `INT_EP_CTRL`, `INT_EP_ADDR_CTRL` and the slot DPRAM buffers are never used. Every pool entry uses `epx_data` as its buffer.
- Why: a slot that receives an IN with the wrong DATA PID never ACKs it, and EPX then stalls as well. Slot polls also report little. On EPX the same toggle error costs one DATA_SEQ, and the device resyncs. See "Measured hardware behaviour".
- Cost: see the load table under "NAK switching and parking".

### Endpoint pool

- `EP_POOL_SIZE` is 32. `ep_pool[0]` is EP0 for every address, and entries 1-31 hold all other endpoints. Upstream had 16 entries (`USB_MAX_ENDPOINTS`): entry 0 for address 0's EP0 and 15 for everything else, each device's EP0 included. Each interrupt endpoint also held one of the 15 hardware slots.
- Opening a control endpoint only records its max packet size in `ep0_mps[]`. On every control transfer, `ep0_select()` points `ep_pool[0]` at the target by setting `dev_addr`, `ep_addr`, `need_pre` and `max_packet_size`. `edpt_find()` returns `ep_pool[0]` for endpoint 0 at any address.
- A shared EP0 relies on usbh running one control transfer at a time. So `hcd_edpt_abort_xfer(addr, 0)` retires whatever control transfer `ep_pool[0]` holds, even one for another address.
- `epx_parked` is a 32-bit mask with one bit per pool entry. A `TU_VERIFY_STATIC` keeps the pool within 32 entries.
- `hcd_free_ep_count()` counts the free entries among 1-31.
- `EPSTATE_WAITING` reuses the value of `EPSTATE_ABORTING`, which only upstream's slot aborts used. `int_interval_ms[]` and `int_due_us[]` are separate arrays indexed by pool entry. Both choices leave upstream's `rp2040_usb.h`, which the device driver shares, uncopied.

### States and dispatch

- Each pool entry is in one of four states:
  - IDLE.
  - PENDING (`PENDING_SETUP` for a SETUP).
  - ACTIVE: it owns EPX and `epx` points at it.
  - WAITING: an interrupt endpoint between polls.
- `epx_switch_ep()` is the only function that starts a transfer on the SIE.
- `hcd_edpt_xfer()` and `hcd_setup_send()` mark the transfer PENDING. An interrupt transfer that is not due yet goes WAITING instead. They start the transfer only if EPX is neither ACTIVE nor held. Otherwise they set `STOP_EPX_ON_NAK`, and the running transfer gives up EPX at its next NAK.
- EPX changes owner only after the SIE has stopped: on completion, failure, retire, or `EPX_STOPPED_ON_NAK`.
- The `EPX_STOPPED_ON_NAK` handler picks the next transfer itself. In every other case, `epx_start_next_pending()` starts the next pending transfer once EPX is idle and not held. It runs at the end of every ISR and after a close or abort. Upstream started the next transfer from inside `xfer_complete_isr()`.
- `epx_next_pending()` scans round robin from the entry after the current one and checks the current entry last. Upstream never checked the current entry. As a result, an endpoint queued again while EPX was held on it still starts.

### NAK switching and parking

Without parking, an always-armed bulk IN that NAKs while idle switches EPX on every NAK. Core 0 load with the FTDI and CH34X adapters attached:

| Scheme | IRQ/s | Core 0 |
|---|---|---|
| Switch on every NAK | 126k | 28% |
| Parking, interrupt endpoints on the slots | 4.2k | 1.3% |
| Parking, everything on EPX (keyboard polled every 1 ms) | 6.2k | 1.7% |

When a bulk or control transfer stops on a NAK, the ISR takes the first case that applies:

1. **An unparked transfer is pending.** The NAKed transfer is parked (its bit is set in `epx_parked`) and EPX switches.
2. **This frame's turn is unused (`epx_nak_turn`).** The next parked transfer after `epx_polled`, in round-robin order, takes EPX and polls until it NAKs. The NAKed transfer is parked and remembered in `epx_turn_from`.
3. **A turn just ended (`epx_turn_from` is set).** EPX goes back to `epx_turn_from` if it is still pending, and the transfer that had the turn is parked.
4. **Nothing else can run.** `STOP_EPX_ON_NAK` is cleared and the same transfer is re-armed, so the hardware keeps polling without an interrupt per NAK.

Cases 2 and 3 fall through to case 4 when they find nothing pending.

In every case, `epx_save_context()` undoes the toggles of buffers that were not sent and rewinds `remaining_len` (and `user_buf` for OUT). The transfer then restarts from buffer 0 when it next runs. In case 4, upstream restarted the SIE with the buffers still armed.

- Parking a transfer enables HOST_SOF. HOST_SOF stays on only while EPX is held, a transfer is parked, or an interrupt transfer waits.
- On each SOF, `sof_service()` drops the parked bits of transfers that are no longer pending. While anything is still parked, it grants one turn and sets `STOP_EPX_ON_NAK`.
- A parked transfer still gets EPX when EPX goes idle. Its bit clears when it takes EPX or is queued again.

### Hold until SOF after a STOP

- Every `STOP_TRANS` goes through `epx_stop()`. It stops the SIE, records the transfer in `epx_stopped`, and calls `epx_hold_until_sof()`.
- `epx_hold_until_sof()` sets `epx_hold`, reads `SOF_RD` to acknowledge any SOF already raised, and enables HOST_SOF. While the hold lasts, nothing starts on EPX and new transfers only queue.
- The next SOF ends the hold in `sof_service()` and settles the stopped transfer in two steps:
  - `epx_undo_unsent()` gives back the PID toggles of buffers that never completed on the wire.
  - `epx_drop_buffers()` clears buffer control and any pending BUFF_STATUS.
- A BUFF_STATUS that arrives while EPX is not ACTIVE is cleared and ignored. The SOF settles its buffer.
- The ISR tests HOST_SOF with a fresh read of INTS, not the snapshot taken on entry. A hold set earlier in the same ISR has just acknowledged that SOF, and that SOF must not end the hold.
- `epx_undo_unsent()` is split out of upstream's `epx_save_context()`. At the SOF it changes only `next_pid`, because by then the stopped transfer has been reported and its endpoint may hold a new transfer.
- When `edpt_release()` frees the `epx_stopped` endpoint, it drops the buffers at once and clears `epx_stopped` without undoing any toggle. The next owner of that pool entry keeps its own `next_pid`.
- `epx_retire()` (abort or close) stops through `epx_stop()` and leaves settling to the SOF. Upstream settled right after the STOP.

Why: a STOP does not end a transaction already on the wire. One more RX_TIMEOUT lands about 4 us after it (see "Measured hardware behaviour"). By the next SOF the transaction has ended.

### Errors

- **STALL_REC:** the transfer fails at once with `XFER_RESULT_STALLED` through `epx_stop()`. The settle at the SOF clears the buffer the STALL leaves and any pending BUFF_STATUS.
- **RX_TIMEOUT:** the third consecutive timeout stops EPX and fails the transfer with `XFER_RESULT_FAILED`. Upstream failed at the first. The controller retries a timed-out transaction on its own, and a NAK in between shows that the device answered.
  - `epx_timeouts` resets on NAK_REC, on any buffer completion, and in every `sie_start_xfer()`.
  - Each ISR gets STALL_REC, RX_TIMEOUT and NAK_REC in one SIE_STATUS read, because NAK_REC has no interrupt of its own. STALL_REC and RX_TIMEOUT act only while EPX is ACTIVE.
  - Clearing an error also clears NAK_REC, so only a NAK seen after that error resets the count.
- **DATA_SEQ_ERROR:** only the latch is cleared. The controller has already ACKed and dropped the packet, and the buffer stays armed. Upstream panicked.
- `sie_start_xfer()` first clears RX_TIMEOUT, STALL_REC, TRANS_COMPLETE, DATA_SEQ_ERROR and EPX_STOPPED_ON_NAK. A latch from an earlier transaction, such as the RX_TIMEOUT that follows a STOP, is never blamed on the new transfer.
- The ISR handles BUFF_STATUS before TRANS_COMPLETE and the error latches, so a packet that completed before an error counts first. If that packet finishes the transfer, the later error is ignored because EPX is no longer ACTIVE. Upstream handled STALL, RX_TIMEOUT and TRANS_COMPLETE first.
- A SETUP's TRANS_COMPLETE counts only while EPX is ACTIVE, so a retired SETUP does not complete EP0.

### Interrupt endpoints

- Interrupt endpoints are scheduled in software. `hcd_edpt_open()` stores bInterval in `int_interval_ms[]`. At full and low speed, bInterval counts frames, which are milliseconds, and 0 counts as 1. A newly opened endpoint is due at once.
- `int_edpt_schedule()` sets the next due time to now + interval - 500 us. It runs on every completion (in `xfer_complete_isr()`) and after every NAKed poll. A poll that ends in the first half of its frame is due again at the SOF one interval later, so a 1 ms endpoint is normally polled every frame.
- A transfer queued before it is due goes WAITING, with HOST_SOF enabled. At each SOF, `sof_service()` makes due transfers PENDING and sets `STOP_EPX_ON_NAK`. A due transfer takes EPX at the running transfer's next NAK or completion. Interrupt transfers are never parked, so they go ahead of parked bulk transfers.
- On EPX, a poll runs with buffer type BULK and `STOP_EPX_ON_NAK` set. A NAKed poll is saved, rescheduled and set back to WAITING until its next interval. EPX passes to the next pending transfer, parked ones included. If none is pending, EPX idles.
- `hcd_edpt_xfer()` treats a due time more than one interval ahead as due. This catches a due time that went stale by more than 2^31 us.

### Root port reset

- The SIE resets the bus itself on attach, so the first `hcd_port_reset()` after an attach does nothing (`root_bus_reset`).
- Any later call drops `USB_PWR.VBUS_DETECT` for `ROOT_RERESET_LOW_US` (1000 us). Such calls come from usbh's root retry, `enum_retry()` or `tuh_rhport_reset_bus()`. The controller then re-runs its debounce and bus reset: DISC, then CONN about 150 ms later, then a fresh attach.
- The DISC's remove event ends the enumeration in flight. If usbh's connect check after the reset runs first, that check ends it instead, because `hcd_port_connect_status()` reports disconnected while `root_rereset` is set. Either way, usbh's root retry count goes back to 0.
- There are at most `ROOT_RERESET_MAX` (3) re-resets per plug-in. The DISC of a re-reset keeps the count, and a real unplug clears it. A call past the cap does nothing, so that retry runs without a bus reset.
- As a result, a failure that repeats on every attempt runs in this order:
  1. The first attach.
  2. Three fresh attaches, one after each re-reset. `enum_retry()` also spends re-resets.
  3. usbh's 3 root retries, without a bus reset.

  After that, nothing happens until the next plug-in.
- The HOST_CONN_DIS handler acknowledges `SIE_STATUS.SPEED` before reading the speed (upstream acknowledged after), so a fast DISC then CONN raises the interrupt again instead of being lost.
- Upstream's `hcd_port_reset()` is a TODO (pico-feedback #387), so usbh's retries got no bus reset.

### Other API

- `hcd_edpt_close()` frees the pool entry and retires any transfer. Upstream returned false. The RIA's MSC and xin drivers call `tuh_edpt_close()`.
- `hcd_edpt_clear_stall()` sets `next_pid` to DATA0. Upstream panicked. The RIA's MSC driver calls it during stall recovery. The halted transfer has already completed or been aborted, so only the toggle changes.
- `hcd_edpt_abort_xfer()` sends every endpoint type through `epx_retire()` and returns true. Upstream's slot abort could return false.
- `hcd_device_close()` retires `ep_pool[0]` only if it points at the closing address, and never frees it. The other entries are freed through `edpt_release()`.
- `hcd_init()` sets `ep_pool[0].dpram_buf` and leaves its type as CONTROL, which keeps EP0 single-buffered (see rp2040_usb.c).

### Measured hardware behaviour

Measured on 2026-11-07 on an RP2350 with a full-speed hub, a low-speed keyboard behind it, FTDI and CH34X serial adapters and a USB stick. Where a measurement contradicts the datasheet, the HCD follows the measurement.

- **Slot, IN with the wrong DATA PID:** the slot never ACKs it, and the device repeats it forever.
  - Meanwhile, EPX takes bogus `FULL|LAST|LEN 0` completions at each poll and later stalls (SIE `SM_STATE` 9, no interrupt).
  - USB, telnet and the main loop's blocking CDC calls then hang until reboot. This was reproduced twice.
  - A slot STALL plus bus errors also stalled EPX.
  - This matches raspberrypi/pico-feedback #394.
- **Slot polls:** they raise no TRANS_COMPLETE, and a poll to an absent address raises nothing.
- **EPX, IN with the wrong DATA PID:** only DATA_SEQ latches. The controller ACKs and drops the packet and raises no BUFF_STATUS. The buffer stays armed, and the next packet arrives normally. This contradicts datasheet 12.7.3.9.2.
- **With every transfer on EPX:** a toggle error on the keyboard or the FTDI gives one DATA_SEQ, and the device resyncs. A halted keyboard endpoint gives one STALLED.
- **EPX RX timeout:** the controller retries every ~4.5 us until `STOP_TRANS`. One more RX_TIMEOUT lands ~4 us after the STOP.
- **Control STALL:** STALL_REC latches, and the EPX buffer reads FULL with LEN 0.
- **`USB_PWR.VBUS_DETECT`:** dropping it, for 1000 us or even 0 us, re-runs the controller's 100 ms debounce and 50 ms bus reset. DISC follows, then CONN about 150 ms later, and everything on the port re-enumerates.
- **`SIE_STATUS.SPEED`:** reads the same before and after it is acknowledged.
- **HOST_SOF:** fires with `KEEP_ALIVE_EN` alone (measured on a full-speed bus). As in upstream, a low-speed root gets only `KEEP_ALIVE_EN` and a full-speed root gets `SOF_EN`. A low-speed keyboard alone on the root port works.
- **EP_RX_ERROR:** works in host mode. The EP0 bits report EPX and the EPn bits report slot n. The HCD does not use it.

### Dropped from upstream

- **RP2040 support:** `HAS_STOP_EPX_ON_NAK`, `epx_switch_request`, `epx_sof_round_robin()` and the NAK_POLL 300/300 delays.
- **Interrupt slots:** `dpram_int_ep_ctrl()`, `dpram_int_ep_buffer_ctrl()`, `int_edpt_status_bit()`, `int_edpt_is_aborting()`, `int_edpt_reclaim()`, `int_edpt_abort()`, `ABORT_FRAME_MASK`, the ABORTING / ABORTING_PENDING abort protocol, slot setup and teardown, and the slot BUFF_STATUS loop.
- **`interrupt_num` and `abort_frame`:** these `hw_endpoint_t` fields stay 0. As a result, rp2040_usb.c's single-buffer rule for host interrupt endpoints no longer applies, and an interrupt transfer longer than one packet may double-buffer on EPX.
- **Other code:**
  - `epx_ctrl_prepare()`, now inlined into `epx_switch_ep()`.
  - `rp2usb_xfer_start()` in `hcd_edpt_xfer()`.
  - The asserts on a free slot and on bInterval > 0.
  - The `pico_trace` in `handle_buf_status_isr()`. It used `buf_status` before its declaration and would not compile at `CFG_TUSB_DEBUG` 3.

## rp2040_usb.c

- `bufctrl_write32()` and `bufctrl_write16()` wait 12 cycles before setting AVAILABLE in host mode too, as datasheet 12.7.3.7.1 requires. Upstream skipped the wait in host mode because the host starts every transaction. However, the controller moves to the second buffer of a double-buffered EPX transfer on its own, and `rp2usb_xfer_continue()` re-arms one half while the controller works on the other.
- `rp2usb_buffer_start()` keeps host control transfers single-buffered. Upstream report #3875 says double-buffered control IN truncates. It has not been reproduced here, but single-buffering EP0 is cheap hardening. The HCD keeps `ep_pool[0]` at type CONTROL for this.

## usbh.c

Apart from the include paths and `queue_event()`, every change concerns failed enumeration. A failure now ends the enumeration instead of stranding it, and it cleans up drivers and the hub port.

- **No free address** (`ENUM_SET_ADDR`): the enumeration fails. With this config, that happens for a 17th device or a 6th hub. Upstream asserted and returned.
- **Configuration descriptor larger than `CFG_TUH_ENUMERATION_BUFSIZE`** (1024, `ENUM_GET_FULL_CONFIG_DESC`): the size is logged and the enumeration fails. Upstream asserted and returned.
- **`enum_parse_configuration_desc()` fails** (`ENUM_CONFIG_DRIVER`): the enumeration fails. The parse fails on an endpoint or interface number at or above `CFG_TUH_ENDPOINT_MAX` or `CFG_TUH_INTERFACE_MAX`, after some drivers may already have opened.
- **New `usbh_driver_set_config_failed(daddr)`:** a class driver whose set_config failed calls it to fail the enumeration. Upstream gives drivers no way to fail set_config, because the return value of `driver->set_config()` is ignored. The function acts only if the device is configured and is the one being enumerated. `usbh_driver_set_config_complete()` uses the same guard, so a late callback cannot fail an enumeration at another address.
- **`enum_teardown()`:** on failure, it calls every class driver's `close(daddr)` before closing the device.
  - Upstream leaked the state that drivers opened at `ENUM_CONFIG_DRIVER`. `clear_device()` clears `connected`, so `remove_device_tree()` later skips the device.
  - Drivers already accept a close for an address they never opened.
- **`enum_full_complete()`, failure behind a hub:**
  - The device's hub port is disabled first with ClearPortFeature(PORT_ENABLE), so the failed device cannot answer the next device's address 0.
  - The completion of that request, `enum_port_disabled()`, re-arms the hub status endpoint.
  - The clear is queued before the next attach starts, and control transfers run in order.
  - If the clear cannot be queued, the status endpoint is re-armed at once, as upstream does.
- **`queue_event()`:** `TU_ASSERT` becomes `TU_VERIFY`. The function runs in the USB IRQ, and an assert's log goes through `rp6502_log_tusb()`, which formats, allocates and prints.

Consequences:

- A device that failed enumeration behind a hub stays disabled until it is replugged or its hub is reset. ClearPortFeature(PORT_ENABLE) raises no port change.
- On the root port, a failure that repeats is retried as described in "Root port reset" under hcd_rp2040.c.

## hub.c

- **`hub_open()`:** rejects addresses in the non-hub range (`dev_addr <= CFG_TUH_DEVICE_MAX`), the same guard `hub_close()` already has. usbh gives out a hub-range address only when bDeviceClass is HUB, but `hub_open()` matches on the interface class. Without the guard, a device with device class 0 and a hub interface would index `hub_itfs[]` out of bounds. Upstream #4200.
- **Status change bitmap:**
  - `hub_edpt_status_xfer()` requests bNbrPorts/8 + 1 bytes. Bit 0 is the hub, and bit N is port N.
  - `hub_xfer_cb()` builds a 32-bit bitmap from the bytes received.
  - Upstream read 1 byte, so ports 8 and up were never seen.
  - `bNbrPorts` is capped at `HUB_PORT_MAX` (31), the most the 4-byte buffer holds. Upstream #4199.
- **GET_HUB_DESCRIPTOR:** sends wValue 0x2900, with descriptor type 0x29 in the high byte (USB 2.0 Table 11-13). Upstream sent 0.
- **Set-config chain** (`hub_set_config()`, `config_get_hub_descriptor()`, `config_set_port_power()`, `config_port_power_complete()`):
  - These failures now call `usbh_driver_set_config_failed()`:
    - a failed GET_HUB_DESCRIPTOR or SET_PORT_POWER;
    - a failure to queue SET_INTERFACE (MTT hubs), GET_HUB_DESCRIPTOR or any SET_PORT_POWER;
    - a failure to arm the status endpoint.
  - The result of the MTT SET_INTERFACE is still not checked.
  - Upstream asserted and returned on the transfer results and on the SET_INTERFACE and GET_HUB_DESCRIPTOR queues. That stranded the enumeration and left the hub's ports unpowered until the hub was unplugged (#4064). Upstream ignored the SET_PORT_POWER queue results. After a failed status-endpoint arm, it completed set_config anyway.
- **Port power chain:** `config_port_power_complete()` ends the chain at `wIndex >= bNbrPorts` (upstream used `==`), so the chain also ends for a hub that reports 0 ports. Port 1 is still powered first, so a 0-port hub that rejects that request fails set_config.

## cdc_host.c

- **`ch34x_process_set_config()`:** a CH34X with chip version below 0x30 fails set_config through `TU_VERIFY`, which logs nothing. `cdch_process_set_config()` then completes set_config as failed, and the device finishes enumerating without that CDC interface. Upstream broke out with no transfer queued, so set_config never completed, which was a strand.

## midi_host.c

This file is a rework of upstream's MIDI host, rebased onto master.

- **`midih_xfer_cb()`:** a failed transfer is not re-armed. Otherwise, a bulk IN on a device that stopped responding would take the shared EPX again after every RX timeout, ahead of the endpoints waiting for it.
  - RX is re-armed only when the application reads data that is still buffered, or on remount.
  - TX resumes at the next write that fills a packet, or at `tuh_midi_write_flush()`.
- **`midih_open()`:**
  - Clears the cable counts and framer state on every open.
  - The descriptor walks also stop on a bLength below 2. `tu_desc_in_bounds()` accepts a bLength of 0, and `tu_desc_next()` never advances past one.
  - The code treats the class-specific endpoint descriptor as optional. bNumEmbMIDIJack is read only from a CS_ENDPOINT descriptor long enough to hold it, and defaults to 1. Upstream failed the open when the endpoint was the last descriptor, and otherwise read whatever descriptor came next.
- **`midih_set_config()`:** an unknown interface completes through `usbh_driver_set_config_complete()`, and enumeration moves on. Upstream asserted and returned with set_config incomplete.
- **`tuh_midi_itf_get_info()`:** checks `idx` against `CFG_TUH_MIDI` before indexing.
- **New `tuh_midi_frame(idx, pkt, out)`:** turns one 4-byte USB-MIDI event packet into up to 3 MIDI 1.0 wire bytes and returns how many it wrote.
  - It tracks running status (`rx_run_len[]`) and sysex (`rx_sysex`) per cable.
  - Real-time bytes pass through inside a sysex. System Reset (0xFF) clears that cable's state.
  - CIN 0 and 1, and cables at or beyond `rx_cable_count`, give 0 bytes.
  - Byte counts come from the status byte, not the CIN.
  - It lives in this file because it keeps its state in the driver's per-interface struct. `src/host/pico/ria/usb/mid.c` calls it.

## RIA config: src/host/pico/ria/tusb_config.h

This file is the RIA's own config, not a copy.

- **`CFG_TUSB_DEBUG_BREAKPOINT`** is an empty function, so a failed assert never executes BKPT. Upstream's breakpoint fires whenever DHCSR.C_DEBUGEN is set. Any OpenOCD session sets that bit, and it stays set until a REBOOT, so a failed assert would halt core 0 on a board that was only being read.
- **Log format strings:** at levels 2 and 3, each one goes in `.flashdata.tusb_log`. The RIA runs from RAM (copy_to_ram), and only `.flashdata*` rodata stays in flash. Level 1 strings (asserts) stay in RAM.
- **`TU_MESS_FAILED`** prints the file name and line number. That costs one RAM literal per file instead of one `__func__` string per function.
- **`CFG_TUSB_DEBUG_PRINTF`** is `rp6502_log_tusb()` (`src/host/pico/host.c`), which feeds the RIA log.
- **Limits:**
  - `CFG_TUH_DEVICE_MAX` is 16, `CFG_TUH_HUB` is 5, and `CFG_TUH_ENUMERATION_BUFSIZE` is 1024.
  - `CFG_TUH_MSC` is 0 because the RIA has its own MSC driver.
  - `CFG_TUH_MIDI_STREAM_API` is 0.

## Upstream references

Hardware reports describe the silicon. Keep these changes whatever TinyUSB does.

| Reference | Change here |
|---|---|
| raspberrypi/pico-feedback #394 | Slots go deaf, so every transfer runs on EPX (hcd_rp2040.c) |
| raspberrypi/pico-feedback #387 | Root re-reset through VBUS_DETECT (hcd_rp2040.c) |

TinyUSB issues: when upstream fixes one, drop the matching change.

| Reference | Change here |
|---|---|
| #3875 | Single-buffered host EP0 (rp2040_usb.c) |
| #4064 | Hub set-config strands (hub.c, usbh.c) |
| #4199 | Hub status bitmap length (hub.c) |
| #4200, PR #4447 | `hub_open()` address guard (hub.c) |

## Re-syncing with upstream

1. Before moving the submodule, save each copy's patch against the current base:
   `diff -u vendor/tinyusb/src/<upstream path> vendor/tinyusb_rp6502/<file> > <file>.patch`
2. Move `vendor/tinyusb` to the new commit. Read upstream's changes to the six files and to the headers they use:
   `git -C vendor/tinyusb diff <old>..<new> -- <paths>`
3. For usbh.c, hub.c, cdc_host.c, rp2040_usb.c and midi_host.c, run `cp vendor/tinyusb/src/<upstream path> vendor/tinyusb_rp6502/<file>`, then `patch vendor/tinyusb_rp6502/<file> < <file>.patch`. Resolve any `.rej` by hand. Drop any change that upstream has fixed (see "Upstream references").
4. For hcd_rp2040.c, about half the file is rewritten. Keep this copy and port the relevant upstream changes into it by hand. These parts are still shared with upstream:
   - `edpt_alloc()`, `dev_speed()` and `need_pre()`
   - `sie_stop_xfer()` and `epx_drop_buffers()`
   - `hcd_deinit()`, `hcd_port_speed_get()` and `hcd_frame_number()`
   - the interrupt enable functions
5. Check what the copies use but do not copy:
   - `rp2040_usb.h`: the `hw_endpoint_t` fields and the `EPSTATE_*` values (`EPSTATE_WAITING` reuses `EPSTATE_ABORTING`).
   - `hcd.h` and `usbh_pvt.h`: the HCD and class driver API.
   - The source list of the `tinyusb_host` library. A renamed or split source must be updated in both lists in `ria.cmake`.
6. Update the base commit at the top of this file.
7. Build the RIA, the RIA-W and the VGA. Also build the Debug RIA-W, which runs out of memory first.
8. Flash the RIA. Check that a hub with a low-speed keyboard, both CDC adapters and a USB stick come up across a few telnet reboots.
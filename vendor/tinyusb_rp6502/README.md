# TinyUSB host overrides

These are modified copies of TinyUSB host files. The RIA builds them instead of the upstream files. They will not go upstream, so code comments stay short and this file holds the reasons.

Upstream base: `vendor/tinyusb` at master `8692556e335a0bf3b0a054a959efd2a3ee677cb6`.

## Build

- `src/host/pico/ria.cmake` marks the six upstream sources `HEADER_FILE_ONLY`, so they are not compiled. It then adds these copies to the RIA target (`rp6502-ria` or `rp6502-ria-w`) with `target_sources`.
- CMake source file properties apply per directory. The VGA builds in `src/host/pico/vga`, so it compiles upstream TinyUSB unmodified, including upstream `rp2040_usb.c` for its device stack.
- hcd_rp2350.c and rp2350_usb.c are copies of upstream's hcd_rp2040.c and rp2040_usb.c. They hold RP2350 host code only, and their names say so, as does the HCD's IRQ handler `hcd_rp2350_irq()`.
- The top-level `CMakeLists.txt` sets `PICO_TINYUSB_PATH` to `vendor/tinyusb`, so the SDK uses the submodule instead of its bundled TinyUSB.
- No headers are copied. The copies include upstream headers by their path under TinyUSB's `src/`, for example `host/usbh_pvt.h` and `portable/raspberrypi/rp2040/rp2040_usb.h`. A new function shared between files is declared in the file that calls it:
  - `hcd_free_ep_count()` (hcd_rp2350.c) is declared in `src/host/pico/ria/usb/usb.c` and feeds the "ep free" status line.
  - `usbh_driver_set_config_failed()` (usbh.c) is declared in hub.c and cdc_host.c.
  - `tuh_control_xfer_drop()` (usbh.c) is declared in `src/host/pico/ria/usb/msc.c`.
  - `tuh_midi_frame()` (midi_host.c) is declared in `src/host/pico/ria/usb/mid.c`.
  - `std_xram_landed()` (`src/core/api/std.c`) is declared in rp2350_usb.c, which calls it.
- The copies carry upstream's license headers; midi_host.c has the full MIT text of the older upstream it was reworked from. No copy is reformatted, so `diff -u` against upstream shows only real changes.

| Copy | Upstream, under `vendor/tinyusb/src/` | Summary |
|---|---|---|
| hcd_rp2350.c | portable/raspberrypi/rp2040/hcd_rp2040.c | Host controller driver. Every transfer runs on EPX, scheduled in software. About half the file is rewritten. |
| rp2350_usb.c | portable/raspberrypi/rp2040/rp2040_usb.c | RP2350 host code only. 12-cycle wait in host mode. Host control and interrupt transfers are single-buffered. A short packet ends a double-buffered IN without a `STOP_TRANS`. |
| usbh.c | host/usbh.c | A failed enumeration is retried, then ends and cleans up instead of stranding. Clears halted endpoints and re-enumerates a device that stops answering. Drops a queued control transfer on request. |
| hub.c | host/hub.c | Fixes set-config strands, reads the multi-byte status bitmap, sends the right descriptor wValue, adds an address guard. Re-enumerates a device whose port the hub disabled and re-powers ports after an over-current. |
| cdc_host.c | class/cdc/cdc_host.c | Fixes the CH34X chip version strand. A failed setup fails the enumeration. Bulk transfers recover after a failure. ACM line requests are always sent. |
| midi_host.c | class/midi/midi_host.c | MIDI host rework, plus the `tuh_midi_frame()` wire framer. Bulk transfers recover after a failure. |

**Strand:** an enumeration failure that returns without ending the enumeration. usbh keeps `enumerating_daddr` set, so every later attach waits. A strand lasts until usbh sees the device, or its hub, unplugged. usbh re-arms a hub's status endpoint only at `ENUM_CONFIG_DRIVER`. So if the device sits behind a hub and fails before that step, the device's own unplug goes unseen, and only unplugging the hub ends the strand.

## hcd_rp2350.c

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

- Every `STOP_TRANS` the driver writes goes through `epx_stop()`. It stops the SIE, records the transfer in `epx_stopped`, and calls `epx_hold_until_sof()`.
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
- Any later call drops `USB_PWR.VBUS_DETECT` for `ROOT_RERESET_LOW_US` (1000 us). Such calls come from `enum_retry()`, usbh's re-attach of a configured device that stops answering, or `tuh_rhport_reset_bus()`. The controller then re-runs its debounce and bus reset: DISC, then CONN about 150 ms later, then a fresh attach.
- The DISC's remove event ends the enumeration in flight. If usbh's connect check after the reset runs first, that check ends it instead, because `hcd_port_connect_status()` reports disconnected while `root_rereset` is set.
- There are at most `ROOT_RERESET_MAX` (3) re-resets per plug-in. The DISC of a re-reset keeps the count, and a real unplug clears it. A call past the cap does nothing, so that retry runs without a bus reset.
- As a result, a failure that repeats on every attempt runs in this order:
  1. The first attach. Its first retry re-resets, which ends it.
  2. Three fresh attaches, one after each re-reset. The first two end the same way.
  3. The third fresh attach runs all 8 attempts (`CFG_TUH_ENUM_ATTEMPT_MAX`). The cap is reached, so its retries get no bus reset.

  That is 11 attempts per plug-in. After that, nothing happens until the next plug-in.
- The HOST_CONN_DIS handler acknowledges `SIE_STATUS.SPEED` before reading the speed (upstream acknowledged after), so a fast DISC then CONN raises the interrupt again instead of being lost.
- Upstream's `hcd_port_reset()` is a TODO (pico-feedback #387), so usbh's retries got no bus reset.

### Other API

- `hcd_edpt_close()` frees the pool entry and retires any transfer. Upstream returned false. The RIA's MSC and xin drivers call `tuh_edpt_close()`.
- `hcd_edpt_clear_stall()` sets `next_pid` to DATA0. Upstream panicked. usbh calls it after a successful CLEAR_FEATURE(ENDPOINT_HALT), and the RIA's MSC driver in its reset recovery. The halted transfer has already completed or been aborted, so only the toggle changes.
- `hcd_edpt_abort_xfer()` sends every endpoint type through `epx_retire()` and returns true. Upstream's slot abort could return false.
- `hcd_device_close()` retires `ep_pool[0]` only if it points at the closing address, and never frees it. The other entries are freed through `edpt_release()`.
- `hcd_init()` sets `ep_pool[0].dpram_buf` and leaves its type as CONTROL, which keeps EP0 single-buffered (see rp2350_usb.c).

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
- **EPX, short packet on a double-buffered IN:** the controller ends the transfer itself, as datasheet 12.7.5 says for `SIE_STATUS.RX_SHORT_PACKET` ("Host: the current transfer will be stopped early"). With the FTDI's reads made two packets long, 3489 short packets ended a double-buffered IN. In the 30 us after each, neither NAK_REC nor ACK_REC latched and the other buffer never went FULL, so no further IN token was sent.
- **Control STALL:** STALL_REC latches, and the EPX buffer reads FULL with LEN 0.
- **`USB_PWR.VBUS_DETECT`:** dropping it, for 1000 us or even 0 us, re-runs the controller's 100 ms debounce and 50 ms bus reset. DISC follows, then CONN about 150 ms later, and everything on the port re-enumerates.
- **`SIE_STATUS.SPEED`:** reads the same before and after it is acknowledged.
- **HOST_SOF:** fires with `KEEP_ALIVE_EN` alone (measured on a full-speed bus). As in upstream, a low-speed root gets only `KEEP_ALIVE_EN` and a full-speed root gets `SOF_EN`. A low-speed keyboard alone on the root port works.
- **EP_RX_ERROR:** works in host mode. The EP0 bits report EPX and the EPn bits report slot n. The HCD does not use it.

### Dropped from upstream

- **RP2040 support:** `HAS_STOP_EPX_ON_NAK`, `epx_switch_request`, `epx_sof_round_robin()` and the NAK_POLL 300/300 delays.
- **Interrupt slots:** `dpram_int_ep_ctrl()`, `dpram_int_ep_buffer_ctrl()`, `int_edpt_status_bit()`, `int_edpt_is_aborting()`, `int_edpt_reclaim()`, `int_edpt_abort()`, `ABORT_FRAME_MASK`, the ABORTING / ABORTING_PENDING abort protocol, slot setup and teardown, and the slot BUFF_STATUS loop.
- **`interrupt_num` and `abort_frame`:** these `hw_endpoint_t` fields stay 0. rp2350_usb.c tests `transfer_type` instead, so host interrupt transfers stay single-buffered.
- **Other code:**
  - `epx_ctrl_prepare()`, now inlined into `epx_switch_ep()`.
  - `rp2usb_xfer_start()` in `hcd_edpt_xfer()`.
  - The asserts on a free slot and on bInterval > 0.
  - The `pico_trace` in `handle_buf_status_isr()`. It used `buf_status` before its declaration and would not compile at `CFG_TUSB_DEBUG` 3.

## rp2350_usb.c

The RIA is an RP2350 USB host and builds no device stack (`CFG_TUD_ENABLED` is 0), so this copy keeps only the host code for the RP2350. The VGA's device stack builds upstream's rp2040_usb.c.

- `bufctrl_write32()` and `bufctrl_write16()` wait 12 cycles before setting AVAILABLE in host mode too, as datasheet 12.7.3.7.1 requires. Upstream skipped the wait in host mode because the host starts every transaction. However, the controller moves to the second buffer of a double-buffered EPX transfer on its own, and `rp2usb_xfer_continue()` re-arms one half while the controller works on the other.
- `rp2usb_buffer_start()` double-buffers bulk transfers only.
  - Control: upstream report #3875 says double-buffered control IN truncates. It has not been reproduced here, but single-buffering EP0 is cheap hardening. The HCD keeps `ep_pool[0]` at type CONTROL for this.
  - Interrupt: upstream single-buffered host interrupt endpoints by testing `interrupt_num`, which the HCD never sets, so this copy tests `transfer_type`. This half has nothing to do with #3875 and stays when upstream fixes that report.
  - Isochronous: upstream single-buffered them by testing the buffer type in `EP_CTRL`. The `transfer_type` test covers them.
- **Received data** (`bufctrl_sync16()`): after it copies each IN packet to the transfer's buffer, it calls `std_xram_landed()`. When the buffer is the range of a running `read_xram`, std.c starts forwarding those bytes to the VGA at once instead of after the whole read returns. Upstream has no such call.
- **Short packet on a double-buffered IN** (`rp2usb_xfer_continue()`): the controller has already ended the transfer (see "Measured hardware behaviour" under hcd_rp2350.c). The branch writes no `STOP_TRANS`. It gives back the PID of the other buffer, which the controller never used, and resets the buffer control. Nothing is on the wire after the short packet, so the next transfer starts at once, with no hold until the SOF.
  - Upstream wrote `STOP_TRANS` and waited for it to clear, outside the HCD's `epx_stop()`. It gave the PID back only when the other buffer was not FULL, which it never is here.

### Dropped from upstream

- **RP2040 errata**, all compiled out on the RP2350 already:
  - E2: `rp2040_chipversion` and its tests around EP_ABORT.
  - E4: reading a single-buffered host completion from buffer 0 when it reports buffer 1.
  - E15: `e15_is_critical_frame_period()`, `e15_last_sof` and the pending state of a device bulk IN.
- **Device code:**
  - `rp2usb_xfer_start()`. Only upstream's HCD and the device driver call it. This HCD starts transfers with `rp2usb_buffer_start()`.
  - The `EPSTATE_IDLE` test in `rp2usb_xfer_continue()`. Upstream's HCD and the device driver can call it on an idle endpoint, after a short packet on a double-buffered IN. This HCD's `handle_buf_status_isr()` calls it only on an ACTIVE transfer.
  - `tu_hwfifo_write()`, `tu_hwfifo_read()` and the FIFO transfer paths (`is_xfer_fifo`, `user_fifo`).
  - The device half of the short-packet branch: EP_ABORT and the `future_len` save.
  - The `ep_reg` NULL tests for device EP0, the `rp2usb_is_host_mode()` tests and the `device/dcd.h` include.

## usbh.c

The changes cover failed enumeration, recovery of configured devices, queued control transfers, and the event queue.

### Failed enumeration

A failure is retried in place. Once the attempts are used up, the enumeration ends and cleans up drivers and the hub port, instead of stranding.

- **No free address** (`ENUM_SET_ADDR`): the enumeration fails. With this config, that happens for a 17th device or a 6th hub. Upstream asserted and returned.
- **Configuration descriptor larger than `CFG_TUH_ENUMERATION_BUFSIZE`** (1024, `ENUM_GET_FULL_CONFIG_DESC`): the size is logged and the enumeration fails. Upstream asserted and returned.
- **`enum_parse_configuration_desc()` fails** (`ENUM_CONFIG_DRIVER`): the enumeration fails. The parse fails on an endpoint or interface number at or above `CFG_TUH_ENDPOINT_MAX` or `CFG_TUH_INTERFACE_MAX`, after some drivers may already have opened.
- **New `usbh_driver_set_config_failed(daddr)`:** a class driver whose set_config failed calls it to fail the enumeration. Upstream gives drivers no way to fail set_config, because the return value of `driver->set_config()` is ignored. The function acts only if the device is configured and is the one being enumerated. `usbh_driver_set_config_complete()` uses the same guard, so a late callback cannot fail an enumeration at another address.
- **One retry for every failure:** every failure exit calls `enum_full_complete(false)`, which calls `enum_retry()` until `CFG_TUH_ENUM_ATTEMPT_MAX` attempts are used up, then `enum_end()`. Upstream retried only from `process_enumeration()` on a FAILED or TIMEOUT control transfer. Its other exits, set_config failures included, ended the enumeration at once.
  - A retry starts again at the debounce delay with a fresh port reset, on the root port and behind a hub alike.
  - Between attempts `enumerating_daddr` stays 0, so no other port enumerates, and attaches stay deferred.
  - The four unplug exits call `enum_end(false)`, which ends the enumeration without a retry. They are the root port's connect checks after the debounce and after the reset, and the hub port's connection checks at `ENUM_HUB_RERSET` and `ENUM_HUB_CLEAR_RESET_COMPLETE`.
  - An enumeration torn down from outside, by an unplug or an attach on its port, is not retried, because `usbh_device_close()` has already cleared `enumerating_daddr`. A late completion from a hub request of the old enumeration can still cost the next attach one attempt.
- **Root port re-post removed:** upstream re-posted a root port attach up to 3 times after a failed enumeration (`USBH_ENUM_ROOT_RETRY_MAX`, `root_retry_count`, `root_retry_rhport`, `attach_teardown`). The in-place retry replaces it, so the root port and hub ports use one mechanism. Upstream gave a hub port at most 2 attempts, the default `CFG_TUH_ENUM_ATTEMPT_MAX`.
- **`enum_teardown()`:** on failure, it calls every class driver's `close(daddr)` before closing the device.
  - Upstream leaked the state that drivers opened at `ENUM_CONFIG_DRIVER`. `clear_device()` clears `connected`, so `remove_device_tree()` later skips the device.
  - Drivers already accept a close for an address they never opened.
- **`enum_end()`, failure behind a hub:**
  - The device's hub port is disabled first with ClearPortFeature(PORT_ENABLE), so the failed device cannot answer the next device's address 0.
  - The completion of that request, `enum_port_disabled()`, re-arms the hub status endpoint, unless `ENUM_CONFIG_DRIVER` already armed it during this attach (`enum_hub_armed`). That arm always has an owner: the status chain re-arms when it finishes, and an attach it posted re-arms when that enumeration ends. Upstream armed again, so after a set_config failure the hub could be armed while the next port enumerated, and the status chain cleared that port's C_PORT_RESET.
  - The clear is queued before the next attach starts, and control transfers run in order.
  - If the clear cannot be queued, the status endpoint is re-armed at once, as upstream does, under the same condition.
- **`ENUM_HUB_CLEAR_RESET`:** the reset counts as done when C_PORT_RESET is set, or when the port reports PORT_ENABLE without PORT_RESET. In the second case there is nothing to clear, and the enumeration goes straight on. Upstream waited 20 ms more and then failed.
  - Why: the hub status endpoint is armed during an enumeration in two cases, a retry after an `ENUM_CONFIG_DRIVER` arm and the re-attach of a configured device (below). It then reports the port's C_PORT_RESET too, and the status chain may clear it first.
- **Hub port status:** the enumeration's GET_STATUS of its port lands in `_usbh_epbuf.ctrl`, and the enumeration reads it there. Upstream used the hub's `ctrl_buf` and `hub_port_get_status_local()`. The status chain writes `ctrl_buf` too, and while the status endpoint is armed (above), its GET_STATUS of another port can complete between the enumeration's GET_STATUS and CLEAR(C_PORT_RESET). `ENUM_HUB_CLEAR_RESET_COMPLETE` then read the other port's status: an empty port ended the enumeration as unplugged, and a low-speed one set the wrong speed. The other way round, the enumeration's GET_STATUS could replace the status the chain reads after its own clear. hub.c's `hub_port_get_status()` writes a port's status to the caller's buffer when there is one.

Consequences:

- A device that failed all its attempts behind a hub stays disabled until it is replugged or its hub is reset. ClearPortFeature(PORT_ENABLE) raises no port change.
- On the root port, each retry goes through `hcd_port_reset()`, see "Root port reset" under hcd_rp2350.c. A re-reset brings a fresh attach, which starts its own attempts.

### Configured devices

- **Halted endpoints (`edpt_clear_halt()`):** when a non-control transfer completes STALLED, usbh first sends CLEAR_FEATURE(ENDPOINT_HALT). Meanwhile the endpoint stays busy and claimed, so the class cannot re-submit early. On success `hcd_edpt_clear_stall()` resets the data toggle. Then the class gets STALLED as before, and its re-arm works. Upstream left the clear to each class, and only MSC did it, so a HID, CDC, MIDI, xinput or hub endpoint stayed halted until a replug.
  - `edpt_halt_cleared()` drops the completion if the device was closed meanwhile. Its `user_data` carries `daddr_gen`, which `usbh_device_close()` bumps before it fails the device's control transfers.
  - If the request cannot be queued (the 4-entry pending control FIFO is full), STALLED is delivered at once. The re-arm stalls again, and the clear is retried.
  - Each STALLED counts toward the re-attach below, so an endpoint that stays halted, or a device that refuses the clear, is enumerated again after 1 s instead of looping through CLEAR_FEATURE and STALL. Each STALL costs the other devices the rest of that frame.
  - The class dispatch moved into `edpt_xfer_complete()`, which both paths use. It clears busy and claimed only on non-control endpoints, because control transfers never set them. A completion without a driver still asserts, but no longer ends the `tuh_task_ext()` run.
- **Re-attach after 1 s of failures:** when a configured device's non-control transfers have failed for `USBH_FAIL_RESET_MS` (1000 ms), usbh handles an attach for the device's own port, as one from its hub or the root port would be handled. That tears the device down and enumerates it again, with a fresh port reset. Upstream left such a device mounted and silent, and each failure idles EPX until the next SOF.
  - A run starts at the first FAILED or STALLED. It ends at any SUCCESS, or when no failure comes for `USBH_FAIL_RESET_MS`. An idle endpoint NAKs, and a NAK completes nothing, so without the gap one transient error would keep a run open until the next one, however much later.
  - Time, not a count, so the reset comes at the same point for any bInterval. Linux `hid_io_error()` does the same for HID.
  - Examples: a device that lost its address (ESD reset, a stray SET_ADDRESS) or sits on a suspended hub port.
  - It acts only while no enumeration runs. Otherwise the run stays open, and the next failure tries again. The attach is handled at once instead of being queued, so it never goes through the deferred-attach queue, which has one entry per hub. Queued, a burst of re-attaches behind a failing hub could overflow it, and the handler would drop an attach after tearing its device down.
  - Hubs count too. usbh never marks a hub mounted, but it is configured. A hub's re-attach removes its whole tree and enumerates it again.
  - On the root port, the reset is a re-reset through VBUS_DETECT, which counts against the 3 per plug-in in "Root port reset" under hcd_rp2350.c. Past those, the re-attach enumerates without a bus reset. A device that lost its address to a reset of its own is at address 0 and still enumerates.
  - `fail_ms` (the run's start, 0 for no run) and `fail_last_ms` (its latest failure) are 16-bit stamps inside the packed device state, so they cost 4 bytes per device.
- **`usbh_device_close()`:** clears `connected` before it fails the device's in-flight control transfer. A callback of that transfer can then no longer queue a request to the departing device, because `tuh_control_xfer()` rejects it. Upstream let a departing hub's callback claim the control slot for the 5 s watchdog. Both callers clear the device right after.

### Queued control transfers

- **New `tuh_control_xfer_drop(daddr, complete_cb, user_data)`:** removes a queued async control transfer that has not started, without calling its callback, and returns true. The first entry in the pending FIFO with the same `daddr`, `complete_cb` and `user_data` goes, and the others keep their order. It returns false when no entry matches: the transfer is in the slot or has completed.
  - Why: `tuh_edpt_abort_xfer(daddr, 0)` aborts only the transfer in the slot. A queued entry keeps the caller's buffer pointer, not a copy, so a request the caller gave up on still goes out later, carrying whatever the buffer then holds, and its completion lands in the caller's next request.
  - msc.c's `msc_cancel_inflight()` calls it for its ADSC, reset-recovery and GET_MAX_LUN requests when a command times out or the device closes. Before, a CB/CBI command that timed out while its ADSC waited behind another device's control transfer left that ADSC queued. It went out with the bytes that recovery wrote into the same buffer, its completion started the dead command's data phase, and the drive stayed wedged until it was replugged.
  - The FIFO is rotated once in place: each entry is read and, unless dropped, written back. No buffer.

### Event queue

- **`queue_event()`:** `TU_ASSERT` becomes `TU_VERIFY`. The function runs in the USB IRQ, and an assert's log goes through `rp6502_log_tusb()`, which formats, allocates and prints.
- **Root attach debounce (`hcd_event_handler()`):** a root port attach that the queue drops clears its debounce bit again. Upstream left the bit set, and then ignored every later root port event.
  - The bit is still set before the attach is queued, as upstream does. Only the USB IRQ raises root port events, so none comes in between. usbh's re-attach of a configured device is handled in the task without an event, see above.

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
- **Port disabled by the hub** (`process_new_status()`): when C_PORT_ENABLE comes with the port connected but not enabled, and a configured device sits on the port, the change is cleared with next state `STATE_CHECK_CONN`. That posts an attach, and usbh tears the device down and enumerates it again. Upstream only cleared the change, so the device stayed mounted and unreachable.
  - A host disable, such as the ClearPortFeature(PORT_ENABLE) that usbh sends after a failed enumeration, sets no C_PORT_ENABLE (USB 2.0 11.24.2.7.2.2).
  - A hub-side disable during an enumeration, such as a port error from a device that babbles, does set it. The status endpoint is not armed then, so the change stays latched until the enumeration has ended. Without the device check, the attach for that change would start all the attempts again, and a device that fails every attempt would never be left disabled. Linux also re-enumerates on C_PORT_ENABLE only when the port has a device.
  - `hub_port_has_device()` looks for the device with `tuh_mounted()` and `tuh_bus_info_get()` over every address, so usbh needs no new function.
- **Over-current** (`process_new_status()`): upstream only cleared the change, and a port that the over-current turned off stayed off until the hub was replugged.
  - Port: after C_PORT_OVER_CURRENT is cleared, `STATE_PORT_POWER` sends SET_FEATURE(PORT_POWER) if the over-current has ended and the port is off.
  - Hub: after C_HUB_OVER_CURRENT is cleared, if the over-current has ended, `STATE_HUB_POWER` sends SET_FEATURE(PORT_POWER) to ports 1 to bNbrPorts in turn. Each step takes the next port from the previous request's wIndex.
  - Either way, an over-current that persists waits for its next change, which comes when it ends.
  - A step of the hub walk that fails goes on to the next port, so one failed request does not leave the remaining ports off.
- **`hub_port_get_status()`:** a port's status goes to the caller's buffer when the caller passes one. Upstream always used the hub's `ctrl_buf`, which the status chain reads after its clears. usbh's enumeration passes its own buffer, see "Hub port status" under usbh.c.
- **Status poll recovery:** the comment drops "This does not repair the status endpoint itself." usbh now clears a halted status endpoint, see usbh.c.

## cdc_host.c

- **`ch34x_process_set_config()`:** a CH34X with chip version below 0x30 fails set_config through `TU_VERIFY`, which logs nothing. `cdch_process_set_config()` then fails set_config, as below. Upstream broke out with no transfer queued, so set_config never completed, which was a strand.
- **`set_config_complete()`, failure:** calls `usbh_driver_set_config_failed()`, so usbh fails the enumeration and retries it. Upstream completed set_config, and the device stayed mounted without that serial port until it was replugged. A single failed vendor setup request, such as the FTDI SIO reset, was enough. A CH34X below version 0x30 fails every attempt.
- **Bulk OUT:** `cdch_xfer_cb()` sends a FAILED or STALLED OUT transfer again, from the first byte the device did not acknowledge. Upstream returned on any result but SUCCESS. The bytes were lost after `tuh_cdc_write()` had reported them written, and the rest of the FIFO waited for the next write.
  - `tx_len` holds the length of the transfer in flight. A transfer from the FIFO starts only in `cdch_write_xfer()`, which records `tx_len`. `tuh_cdc_write_flush()` and `cdch_xfer_cb()` call it. The resend in `cdch_xfer_cb()` restarts the unsent tail itself and lowers `tx_len`. A ZLP sets it to 0.
  - `tuh_cdc_write()` only fills the FIFO, with `tu_fifo_write_n()`. Upstream's `tu_edpt_stream_write()` also starts a transfer once the FIFO holds a packet, and tusb.c does not pass that transfer's length back. vcp.c flushes after every write.
  - A resend is safe. The HCD gives back the toggles of packets it did not send, and the device drops a repeated packet by its toggle.
  - STALLED is handled like FAILED, because usbh clears the halt before it reports STALLED.
- **Bulk IN:** `cdch_xfer_cb()` keeps the bytes received and re-arms on every result. Upstream did neither after a failure, so input stopped until the device was replugged.
  - The FTDI status bytes are stripped on every result, because every FTDI packet starts with them.
  - A device that stops answering fails one transfer per frame, and each failure costs the other devices the rest of that frame, until usbh re-enumerates it after 1 s of failures.
- **CDC ACM line requests:** `acm_set_line_coding()` and `acm_set_control_line_state()` always send, as Linux does. Upstream sent neither when the ACM functional descriptor leaves the line-request bit of bmCapabilities clear, and the call failed. Some devices leave the bit clear and accept the requests anyway. A device without them answers with a STALL, which vcp.c counts as success.
  - `acm_open()` does not store bmCapabilities, and the driver union has no `acm` member, because nothing reads them. The walk still skips the functional descriptors.
- **Two-stage line coding** (`cdch_set_line_coding_stage1_baudrate_complete()`): when the data format request cannot start, the caller's callback gets FAILED. FTDI, for example, refuses 5 and 6 data bits. Upstream asserted and returned without calling back, so vcp.c would have kept the port configuring forever.

## midi_host.c

This file is a rework of upstream's MIDI host, rebased onto master.

- **`midih_xfer_cb()`:** a FAILED or STALLED OUT transfer is sent again from the first byte the device did not acknowledge, as in cdc_host.c. Upstream treated every result as a completion, so the packets of a failed transfer were lost, which can leave a note stuck on.
  - `tx_len` holds the length of the transfer in flight. A transfer from the FIFO starts only in `midih_write_xfer()`, which records `tx_len`. `tuh_midi_write_flush()` and `midih_xfer_cb()` call it. The resend in `midih_xfer_cb()` restarts the unsent tail itself and lowers `tx_len`. A ZLP sets it to 0.
  - `tuh_midi_packet_write_n()` and the stream API only fill the FIFO, with `tu_fifo_write_n()`, for the reason given under cdc_host.c. mid.c flushes every interface it wrote to.
  - IN is re-armed on every result, as upstream does. A device that stops answering fails one IN per frame until usbh re-enumerates it after 1 s of failures.
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
- **`CFG_TUH_TASK_QUEUE_SZ`** is 64 (upstream 16), more than all producers can have in flight at once. A dropped completion leaves its endpoint busy and claimed for good, and a dropped hub attach leaves that hub unarmed. The 48 extra 12-byte events cost 576 B. It also lets usbh's control watchdog wait out up to 64 queued events, instead of 16, before it fires under sustained traffic.
- **`CFG_TUH_ENUM_ATTEMPT_MAX`** is 8 (upstream 2). usbh retries every enumeration failure in place, on the root port and behind a hub (see usbh.c). Each attempt costs the 150 ms debounce plus the reset and its recovery. A device that fails every attempt holds up the other ports' attaches meanwhile, up to 8 control timeouts of 5 s for one that NAKs a request forever.

## Upstream references

Hardware reports describe the silicon. Keep these changes whatever TinyUSB does.

| Reference | Change here |
|---|---|
| raspberrypi/pico-feedback #394 | Slots go deaf, so every transfer runs on EPX (hcd_rp2350.c) |
| raspberrypi/pico-feedback #387 | Root re-reset through VBUS_DETECT (hcd_rp2350.c) |

TinyUSB issues: when upstream fixes one, drop the matching change.

| Reference | Change here |
|---|---|
| #3875 | Single-buffered host EP0 (rp2350_usb.c). Interrupt transfers stay single-buffered. |
| #4064 | Hub set-config strands (hub.c, usbh.c) |
| #4199 | Hub status bitmap length (hub.c) |
| #4200, PR #4447 | `hub_open()` address guard (hub.c) |

## Re-syncing with upstream

`<file>` is a copy's name and `<upstream path>` its upstream file, from the table at the top. hcd_rp2350.c and rp2350_usb.c are named differently from their upstream files.

1. Before moving the submodule, save each copy's patch against the current base:
   `diff -u vendor/tinyusb/src/<upstream path> vendor/tinyusb_rp6502/<file> > <file>.patch`
2. Move `vendor/tinyusb` to the new commit. Read upstream's changes to the six files and to the headers they use:
   `git -C vendor/tinyusb diff <old>..<new> -- <paths>`
3. For usbh.c, hub.c, cdc_host.c, midi_host.c and rp2350_usb.c, run `cp vendor/tinyusb/src/<upstream path> vendor/tinyusb_rp6502/<file>`, then `patch vendor/tinyusb_rp6502/<file> < <file>.patch`. Resolve any `.rej` by hand. Drop any change that upstream has fixed (see "Upstream references"). A reject in rp2350_usb.c that touches device or RP2040 errata code is resolved by deleting that code.
4. For hcd_rp2350.c, about half the file is rewritten. Keep this copy and port the relevant upstream changes to hcd_rp2040.c into it by hand. These parts are still shared with upstream:
   - `edpt_alloc()`, `dev_speed()` and `need_pre()`
   - `sie_stop_xfer()` and `epx_drop_buffers()`
   - `hcd_deinit()`, `hcd_port_speed_get()` and `hcd_frame_number()`
   - the interrupt enable functions
5. Check what the copies use but do not copy:
   - `rp2040_usb.h`: the `hw_endpoint_t` fields and the `EPSTATE_*` values (`EPSTATE_WAITING` reuses `EPSTATE_ABORTING`).
   - `hcd.h` and `usbh_pvt.h`: the HCD and class driver API.
   - The source list of the `tinyusb_host` library. The `HEADER_FILE_ONLY` list in `ria.cmake` names upstream's files and must follow a rename or split upstream. `target_sources` names the copies.
6. Update the base commit at the top of this file.
7. Build the RIA, the RIA-W and the VGA. Also build the Debug RIA-W, which runs out of memory first.
8. Flash the RIA. Check that a hub with a low-speed keyboard, both CDC adapters and a USB stick come up across a few telnet reboots.
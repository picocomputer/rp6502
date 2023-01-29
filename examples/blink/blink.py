import sys
sys.path.insert(1, '../')
import rp6502

tty = '/dev/ttyACM0'
upload = 'blink.rp6502'
run = ['cl65',
       '-t', 'none',
       '--cpu', '65c02',
       '-o', 'blink.bin',
       'blink.s']

if len(sys.argv) == 2 and sys.argv[1].lower() == "run":
    rp6502.run(run)
    mon=rp6502.Monitor(tty)
    mon.send_break()
    mon.send_file_to_memory('blink.bin', 0x200)
    mon.send_reset_vector()
    mon.reset()

if len(sys.argv) == 2 and sys.argv[1].lower() == "upload":
    rp6502.run(run)
    rom=rp6502.ROM()
    rom.comment("Blink - A light show by R.D.Thumps")
    rom.binary_file('blink.bin', 0x200)
    rom.reset_vector()
    mon=rp6502.Monitor(tty)
    mon.send_break()
    mon.upload(upload, rom)

else:
    print("Usage: python3 blink.py run|upload")

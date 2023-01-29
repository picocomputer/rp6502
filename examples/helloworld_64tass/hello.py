import sys
sys.path.insert(1, '../')
import rp6502

tty = '/dev/ttyACM0'
upload = 'hello.rp6502'
run = ['64tass', '--mw65c02', 'hello.s']

if len(sys.argv) == 2 and sys.argv[1].lower() == "run":
    rp6502.run(run)
    mon=rp6502.Monitor(tty)
    mon.send_break()
    mon.send_file_to_memory('a.out')
    mon.send_reset_vector()
    mon.reset()

if len(sys.argv) == 2 and sys.argv[1].lower() == "upload":
    rp6502.run(run)
    rom=rp6502.ROM()
    rom.comment("Hello World - Compiled by 64tass Turbo Assembler")
    rom.binary_file('a.out')
    rom.reset_vector()
    mon=rp6502.Monitor(tty)
    mon.send_break()
    mon.upload(upload, rom)

else:
    print("Usage: python3 hello.py run|upload")

# Example for controlling RP6502 RIA via UART

import sys,subprocess,serial,binascii
ser = serial.Serial()

def run(args) -> subprocess.CompletedProcess:
    cp = subprocess.run(args)
    if cp.returncode != 0:
        sys.exit()
    return cp

def device(name):
    ser.setPort(name)
    ser.timeout = 0.2
    ser.open()

def reset():
    ''' Stop the 6502 and return to monitor. '''
    ser.send_break(0.1)
    ser.read_all()
    ser.read_until(']')

def write(str):
    ''' Send anything. Does not read reply. '''
    ser.write(bytes(str, 'utf-8'))

def monitor_command(str):
    ''' Send one line and wait for next monitor prompt '''
    ser.write(bytes(str, 'utf-8'))
    ser.write(b'\r')
    while True:
        r = ser.read()
        if r == b'?':
            print(']'+str)
            print('?'+ser.read_until().decode('utf-8').strip())
            sys.exit()
        if r == b']':
            break

def send_binary(addr, data):
    ''' Send data to memory using fast BINARY command '''
    command = f'BINARY ${addr:04X} ${len(data):04X} ${binascii.crc32(data):08X}\r'
    ser.write(bytes(command, 'utf-8'))
    ser.write(data)
    while True:
        r = ser.read()
        if r == b'?':
            print(']'+command)
            print('?'+ser.read_until().decode('utf-8').strip())
            sys.exit("Error")
        if r == b']':
            break

def send_file_to_memory(name, addr=None):
    ''' Send binary file. addr=None uses first two bytes as address.'''
    with open(name, 'rb') as f:
        data = f.read()
    pos = 0
    if addr==None:
        pos += 2
        addr = data[0] + data[1] * 256
        # guess reset vector
        monitor_command(f"FFFC: {data[0]:02X} {data[1]:02X}")
    while pos < len(data):
        size = len(data) - pos
        if size > 1024:
            size = 1024
        send_binary(addr, data[pos:pos+size])
        addr += size
        pos += size

run(['64tass', '--mw65c02', 'hello.s'])
device('/dev/ttyACM0')
reset()
send_file_to_memory('a.out')
monitor_command('start')

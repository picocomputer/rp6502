# Example for controlling RP6502 RIA via UART

import sys,subprocess,serial
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
    while ser.in_waiting:
        ser.read()

def send(str):
    ''' Send one line using "0\b" faux flow control '''
    ser.write(b'0')
    while True:
        r = ser.read()
        if r == b'\x00': # huh, zeros?
            continue
        if r == b'0':
            break
        sys.exit("Error")
    ser.write(b'\b')
    ser.write(bytes(str, 'utf-8'))
    ser.write(b'\r')
    ser.read_until()

def write(str):
    ''' Send anything. Does not read reply. '''
    ser.write(bytes(str, 'utf-8'))

def send_textfile(name):
    ''' Send text file. '''
    f = open(name, 'r')
    for line in f:
        send(line.strip())
    f.close()

def send_binfile(name, addr=None):
    ''' Send text file. addr=None uses first two bytes as address.'''
    with open(name, 'rb') as f:
        data = f.read()
    pos = 0
    if addr==None:
        addr = data[0] + data[1] * 256
        pos += 2
    out = ''
    while pos < len(data):
        if not out:
            out = f'{addr:04X}:'
        out += f' {data[pos]:02X}'
        addr += 1
        pos += 1
        if pos == len(data) or not (addr & 0xF):
            send(out)
            out = ''

def ready():
    ''' Wait for BASIC Ready '''
    while True:
        if b'Ready\r\n' == ser.readline():
            break

run(['64tass', '--mw65c02', 'hello.s'])
device('/dev/ttyACM0')
reset()
send_binfile('a.out')
send('jmp $200')

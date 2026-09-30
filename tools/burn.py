"""Send a 512 KB card image to the SST programmer over USB and (optionally) burn it.

  python burn.py card.bin                # store on the programmer
  python burn.py card.bin --burn         # store, then erase + burn the SST
  python burn.py --list                  # show stored cards
  python burn.py --burn-stored name.bin  # burn a card already stored
  python burn.py --delete name.bin
  python burn.py --pin A5                # pin test: raise one line, meter the SST pin (--pin OFF to release)
Port is found automatically (CH343 USB-serial); force one with --port COM19.
"""
import argparse, os, sys, time, zlib
import serial
from serial.tools import list_ports

BAUD = 921600
BLOCK = 4096


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x1A86:                        # WCH CH343 / CH340 on the S3 DevKit's UART port
            return p.device
    sys.exit('no CH343 USB-serial port found - plug in the UART USB-C port (or pass --port)')


def line(ser, timeout=5):
    ser.timeout = timeout
    return ser.readline().decode('utf-8', 'replace').strip()


def expect(ser, prefix, timeout=5):
    """skip debug lines until one starts with prefix"""
    end = time.time() + timeout
    while time.time() < end:
        l = line(ser, max(0.1, end - time.time()))
        if l.startswith(prefix):
            return l
        if l.startswith('ERR'):
            sys.exit('programmer said: ' + l)
    sys.exit('timeout waiting for ' + prefix)


def cmd(ser, text, prefix):
    ser.reset_input_buffer()
    ser.write((text + '\n').encode())
    return expect(ser, prefix)


def put(ser, path, info=None):
    data = open(path, 'rb').read()
    if len(data) != 512 * 1024:
        sys.exit('%s is %d bytes, must be exactly 524288 (512 KB)' % (path, len(data)))
    name = os.path.basename(path).replace(' ', '_')
    crc = zlib.crc32(data) & 0xFFFFFFFF
    cmd(ser, 'PUT %s %d %x' % (name, len(data), crc), 'OK')
    t0 = time.time()
    for i in range(0, len(data), BLOCK):
        ser.write(data[i:i + BLOCK])
        ack = ser.read(1)
        if ack != b'K':
            sys.exit('block %d: no ack (%r)' % (i // BLOCK, ack))
        if (i // BLOCK) % 16 == 0:
            print('\r  sending %3d%%' % (100 * i // len(data)), end='', flush=True)
    r = expect(ser, 'DONE', 10)
    print('\r  sent %s (%d KB) in %.1f s, CRC ok' % (name, len(data) // 1024, time.time() - t0))
    return name


def burn(ser, name):
    cmd(ser, 'BURN ' + name, 'OK')
    print('  burning', name)
    while True:
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(b'STATUS\n')
        l = expect(ser, 'STATUS', 5).split(' ', 4)
        state, done, total = int(l[1]), int(l[2]), int(l[3])
        msg = l[4] if len(l) > 4 else ''
        names = ['idle', 'erasing chip', 'writing', 'DONE', 'ERROR']
        print('\r  %-13s %3d%%' % (names[state], 100 * done // total), end='', flush=True)
        if state in (3, 4):
            print('\n  ' + msg)
            return state == 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?')
    ap.add_argument('--port')
    ap.add_argument('--burn', action='store_true')
    ap.add_argument('--info', help='text shown next to the card on the web page (name, tone names)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--burn-stored')
    ap.add_argument('--delete')
    ap.add_argument('--pin', help='pin test: A0..A18, D0..D7, WE or OFF - raises that line so you can meter the SST pin')
    a = ap.parse_args()
    ser = serial.Serial()
    ser.port, ser.baudrate = a.port or find_port(), BAUD
    ser.dtr = ser.rts = False                      # opening the port must not reset the board
    ser.open()
    time.sleep(0.3)
    if not cmd(ser, 'PING', 'PONG'):
        sys.exit('no answer')
    if a.list:
        ser.write(b'LIST\n')
        while True:
            l = line(ser)
            if l.startswith('FILE'):
                print(l[5:])
            if l == 'END' or not l:
                break
    elif a.pin:
        print(cmd(ser, 'PIN ' + a.pin, 'OK'))
    elif a.delete:
        print(cmd(ser, 'DEL ' + a.delete, 'OK'))
    elif a.burn_stored:
        sys.exit(0 if burn(ser, a.burn_stored) else 1)
    elif a.file:
        name = put(ser, a.file, a.info)
        if a.burn:
            sys.exit(0 if burn(ser, name) else 1)
    else:
        ap.print_help()


if __name__ == '__main__':
    main()

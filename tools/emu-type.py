#!/usr/bin/env python3
# Type text into the emulated guest through the QEMU monitor's sendkey.
#
#   emu-type.py <monitor.sock> <text>        (\n in the text is Enter)
#
# US layout; covers what a shell command needs. Used by tools/emu-linux.sh to
# run diagnostic-log.sh in the Recovery Terminal.
import socket
import sys
import time

NAMES = {
    ' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '_': 'shift-minus', '*': 'shift-8',
    '|': 'shift-backslash', '>': 'shift-dot', '<': 'shift-comma', '=': 'equal',
    ':': 'shift-semicolon', ';': 'semicolon', '"': 'shift-apostrophe', "'": 'apostrophe',
    '$': 'shift-4', '&': 'shift-7', '(': 'shift-9', ')': 'shift-0', '\n': 'ret', ',': 'comma',
    '~': 'shift-grave_accent', '#': 'shift-3', '%': 'shift-5', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', '!': 'shift-1', '?': 'shift-slash',
    '+': 'shift-equal', '@': 'shift-2', '{': 'shift-bracket_left', '}': 'shift-bracket_right',
}


def key(c):
    if c in NAMES:
        return NAMES[c]
    return 'shift-' + c.lower() if c.isupper() else c


def main():
    sock, text = sys.argv[1], sys.argv[2].replace('\\n', '\n')
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    time.sleep(0.3)
    s.recv(65536)
    for c in text:
        s.send(('sendkey %s 40\n' % key(c)).encode())
        time.sleep(0.07)
    time.sleep(0.5)


main()

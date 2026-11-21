// The device's MIDI ports: the MIDI IN/OUT jacks and USB MIDI.
#pragma once

enum { MIDI_PORT_EXT, MIDI_PORT_USB, MIDI_PORTS };

int midi_port_open(int which); // 0 when the port is there, for reading or writing
void midi_port_close(int which); // USB mode: the USB MIDI port let go of, opened again after
// Which ports are switched on (MIDI Settings: Ext MIDI, USB MIDI). Off, a port sends nothing
// and what comes in on it is dropped.
void midi_ports_enable(int ext, int usb);
void midi_out_send(const unsigned char *m, int n); // to every port switched on. Never blocks.
// The next message in: 1 with it in m (two-byte ones end in 0) and its port in *from, or 0.
// Clock, start, continue and stop come through too, alone in m[0].
int midi_in_read(unsigned char m[3], int *from);

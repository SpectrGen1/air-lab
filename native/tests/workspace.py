"""Two Wayland windows with a real GTK data-device drag and drop."""
import os
from pathlib import Path

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
from gi.repository import Gdk, GLib, Gtk

events = Path(os.environ['AIR_TEST_EVENTS'])
drawings = []
frame = 0
targets = [Gtk.TargetEntry.new('text/plain', 0, 0)]


def emit(event):
    with events.open('a') as log:
        print(event, file=log)


def animation_tick():
    global frame
    frame += 1
    for drawing in drawings:
        drawing.queue_draw()
    return True


def key_pressed(_, event, title):
    emit(f'{title} key {Gdk.keyval_name(event.keyval)}')
    if event.keyval == Gdk.KEY_F6 and not frame:
        animation_tick()
        GLib.timeout_add(16, animation_tick)
        emit('Animation started')
        return True
    return False


def draw(widget, cr):
    if not frame:
        return
    w, h = widget.get_allocated_width(), widget.get_allocated_height()
    for index in range(16):
        cr.set_source_rgb(.16 + ((frame + index * 5) % 31) / 63, .23, .35)
        cr.rectangle(0, index * h / 16, w, h / 16 + 1)
        cr.fill()


def window(title):
    top = Gtk.Window(title=title)
    top.set_default_size(900, 600)
    top.connect('key-press-event', key_pressed, title)
    area = Gtk.EventBox()
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    entry = Gtk.Entry()
    entry.set_placeholder_text(title)
    box.pack_start(entry, False, False, 0)
    drawing = Gtk.DrawingArea()
    drawing.connect('draw', draw)
    drawings.append(drawing)
    box.pack_start(drawing, True, True, 0)
    area.add(box)
    top.add(area)
    return top, area


source, source_area = window('Source')
destination, destination_area = window('Destination')
source_area.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, targets, Gdk.DragAction.COPY)
source_area.connect('drag-data-get', lambda _, _context, data, _info, _time:
                    data.set_text('air-workspace-test', -1))
source_area.connect('drag-begin', lambda *_: emit('Source drag started'))
destination_area.drag_dest_set(Gtk.DestDefaults.ALL, targets, Gdk.DragAction.COPY)
destination_area.connect('drag-data-received', lambda _, _context, _x, _y, data, _info, _time:
                         emit(f'Destination drop {data.get_text()}'))
source.show_all()
destination.show_all()
emit('ready')
Gtk.main()

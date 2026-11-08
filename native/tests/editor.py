import gi, json,time,os
from pathlib import Path
gi.require_version('Gtk','3.0')
from gi.repository import Gtk,Gdk,GLib
log=Path(os.environ['AIR_TEST_EVENTS'])
def emit(kind,**kw):
 with log.open('a') as f:f.write(json.dumps({'time':time.monotonic(),'kind':kind,**kw})+'\n')
w=Gtk.Window(title='Native Spatial Editor Test');w.set_wmclass('air-native-editor','air-native-editor');w.set_default_size(900,600)
t=Gtk.TextView();t.set_monospace(True);t.get_buffer().set_text('Original labwc client — native spatial integration\n\nClick here, type, and drag to select text across the panel edge.\nHead turns must never move this editor pointer or steal keyboard focus.\n');box=Gtk.Box(orientation=Gtk.Orientation.VERTICAL);label=Gtk.Label(label="Frame callback check 0");box.pack_start(label,False,False,0);box.pack_start(t,True,True,0);w.add(box)
counter=[0]
def animate():
 counter[0]+=1;label.set_text("Frame callback check "+str(counter[0]));return True
GLib.timeout_add(120,animate)
t.add_events(Gdk.EventMask.SCROLL_MASK|Gdk.EventMask.POINTER_MOTION_MASK|Gdk.EventMask.BUTTON_PRESS_MASK|Gdk.EventMask.BUTTON_RELEASE_MASK|Gdk.EventMask.ENTER_NOTIFY_MASK|Gdk.EventMask.LEAVE_NOTIFY_MASK)
t.connect('scroll-event',lambda widget,e:emit('scroll') or False)
t.connect('motion-notify-event',lambda widget,e:emit('motion',x=e.x,y=e.y) or False)
t.connect('enter-notify-event',lambda widget,e:emit('enter',x=e.x,y=e.y) or False)
t.connect('leave-notify-event',lambda widget,e:emit('leave') or False)
t.connect('button-press-event',lambda widget,e:emit('press',button=e.button,x=e.x,y=e.y) or False)
t.connect('button-release-event',lambda widget,e:emit('release',button=e.button,x=e.x,y=e.y) or False)
def keypress(widget,e):
 emit('key',key=e.keyval)
 if e.keyval == Gdk.KEY_F3:
  other=Gtk.Window(title='Spatial lifecycle test');other.set_default_size(900,600)
  other.add(Gtk.Label(label='Close this window while its spatial frame is held.'))
  def close_other(widget,event):
   if event.keyval == Gdk.KEY_Escape:
    emit('lifecycle-closed');other.destroy();return True
   return False
  other.connect('key-press-event',close_other);other.show_all();emit('lifecycle-ready');return True
 if e.keyval == Gdk.KEY_F2:
  dialog=Gtk.Dialog(title='Native modal test',transient_for=w,flags=Gtk.DialogFlags.MODAL)
  dialog.add_button('Close',Gtk.ResponseType.CLOSE)
  dialog.get_content_area().add(Gtk.Label(label='Transient dialog stays with its parent.'))
  dialog.connect('response',lambda d,response:(emit('dialog-closed'),d.destroy()))
  dialog.show_all();emit('dialog-ready');return True
 return False
t.connect('key-press-event',keypress)
t.get_buffer().connect('changed',lambda b:emit('text',value=b.get_text(b.get_start_iter(),b.get_end_iter(),True)))
w.connect('destroy',Gtk.main_quit);w.show_all();emit('ready');Gtk.main()

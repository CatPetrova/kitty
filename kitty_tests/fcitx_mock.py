# License: GPLv3

import sys
from pathlib import Path


def main():
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib

    service = 'org.freedesktop.portal.Fcitx'
    im_interface = 'org.fcitx.Fcitx.InputMethod1'
    context_interface = 'org.fcitx.Fcitx.InputContext1'
    portal_path = '/org/freedesktop/portal/inputmethod'
    journal = Path(sys.argv[1])

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(service, bus=bus, do_not_queue=True)

    class InputContext(dbus.service.Object):
        def __init__(self, path):
            super().__init__(name, path)
            self.path = path
            self.composing = False

        def record(self, event):
            with journal.open('a', encoding='utf-8') as f:
                f.write(f'{event} {self.path}\n')

        @dbus.service.method(context_interface, in_signature='t')
        def SetCapability(self, capabilities):
            pass

        @dbus.service.method(context_interface, in_signature='iiii')
        def SetCursorRect(self, left, top, width, height):
            pass

        @dbus.service.method(context_interface)
        def FocusIn(self):
            self.record('focus')

        @dbus.service.method(context_interface)
        def FocusOut(self):
            if self.composing:
                self.CommitString('未取消的预编辑')
                self.composing = False

        @dbus.service.method(context_interface)
        def Reset(self):
            self.record('reset')
            if self.composing:
                # Emulate a signal already in transit from the retired context.
                self.CommitString('迟到的提交')
                self.composing = False

        @dbus.service.method(context_interface)
        def DestroyIC(self):
            self.remove_from_connection()

        @dbus.service.method(context_interface, in_signature='uuubu', out_signature='b')
        def ProcessKeyEvent(self, keysym, keycode, state, released, timestamp):
            if not released:
                if keysym == ord('n'):
                    self.composing = True
                    self.UpdateFormattedPreedit([('你', 0), ('好', 0)], len('你'.encode()))
                    return True
                if keysym == ord(' ') and self.composing:
                    self.composing = False
                    self.CommitString('你好')
                    return True
            return False

        @dbus.service.signal(context_interface, signature='s')
        def CommitString(self, text):
            pass

        @dbus.service.signal(context_interface, signature='a(si)i')
        def UpdateFormattedPreedit(self, text, cursor):
            pass

    class Portal(dbus.service.Object):
        context_id = 0

        @dbus.service.method(im_interface, in_signature='a(ss)', out_signature='oay')
        def CreateInputContext(self, properties):
            self.context_id += 1
            path = f'/org/freedesktop/portal/inputcontext/{self.context_id}'
            InputContext(path)
            return path, dbus.ByteArray(bytes(16))

    portal = Portal(name, portal_path)
    print('ready', flush=True)
    GLib.MainLoop().run()
    portal.remove_from_connection()


if __name__ == '__main__':
    main()

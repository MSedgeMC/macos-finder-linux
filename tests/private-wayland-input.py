"""Input helper for private virtual-screen tests only; never a working session."""
import os
class NativeInput:
    def __init__(self):
        assert os.environ.get("KUBUNTU_VIRTUAL_SCREEN")=="1"
        assert os.environ.get("WAYLAND_DISPLAY")=="codex-test-screen"
        import ctypes as C
        self.C=C
        class Message(C.Structure):
            _fields_=[('name',C.c_char_p),('signature',C.c_char_p),('types',C.c_void_p)]
        class Interface(C.Structure):
            _fields_=[('name',C.c_char_p),('version',C.c_int),('method_count',C.c_int),('methods',C.POINTER(Message)),('event_count',C.c_int),('events',C.POINTER(Message))]
        lib=self.lib=C.CDLL('libwayland-client.so.0')
        lib.wl_display_connect.argtypes=[C.c_char_p];lib.wl_display_connect.restype=C.c_void_p
        lib.wl_proxy_marshal_flags.argtypes=[C.c_void_p,C.c_uint,C.POINTER(Interface),C.c_uint,C.c_uint];lib.wl_proxy_marshal_flags.restype=C.c_void_p
        lib.wl_proxy_add_listener.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p]
        for name in ['wl_display_roundtrip','wl_display_disconnect','wl_display_flush']:getattr(lib,name).argtypes=[C.c_void_p]
        self.display=lib.wl_display_connect(None);assert self.display
        requests=[(b'authenticate',b'ss'),(b'pointer_motion',b'ff'),(b'button',b'uu'),(b'axis',b'uf'),(b'touch_down',b'2uff'),(b'touch_motion',b'2uff'),(b'touch_up',b'2u'),(b'touch_cancel',b'2'),(b'touch_frame',b'2'),(b'pointer_motion_absolute',b'3ff'),(b'keyboard_key',b'4uu'),(b'destroy',b'5'),(b'keyboard_keysym',b'6uu')]
        self.messages=(Message*len(requests))(*(Message(n,s,None) for n,s in requests))
        self.interface=Interface(b'org_kde_kwin_fake_input',6,len(requests),self.messages,0,None)
        registry_type=Interface.in_dll(lib,'wl_registry_interface')
        self.registry=lib.wl_proxy_marshal_flags(self.display,1,C.byref(registry_type),1,0,C.c_void_p())
        added=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p,C.c_uint,C.c_char_p,C.c_uint)
        removed=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p,C.c_uint)
        def bind(data,registry,name,interface,version):
            if interface==b'org_kde_kwin_fake_input':
                self.version=min(version,6)
                self.proxy=lib.wl_proxy_marshal_flags(registry,0,C.byref(self.interface),self.version,0,C.c_uint(name),interface,C.c_uint(self.version),C.c_void_p())
        self.added=added(bind);self.removed=removed(lambda *args:None)
        self.listeners=(C.c_void_p*2)(C.cast(self.added,C.c_void_p),C.cast(self.removed,C.c_void_p))
        lib.wl_proxy_add_listener(self.registry,self.listeners,None);lib.wl_display_roundtrip(self.display)
        for _ in range(4):
            if hasattr(self,'proxy'):break
            lib.wl_display_roundtrip(self.display)
        assert hasattr(self,'proxy'),'Private fake-input interface unavailable'
        self.send(0,C.c_char_p(b'Minimize geometry test'),C.c_char_p(b'Private input-region verification'));lib.wl_display_roundtrip(self.display)
    def send(self,opcode,*args):
        self.lib.wl_proxy_marshal_flags(self.proxy,opcode,None,self.version,0,*args);self.lib.wl_display_flush(self.display)
    def move(self,x,y):self.send(9,self.C.c_int(int(x*256)),self.C.c_int(int(y*256)))
    def button(self,state):self.send(2,self.C.c_uint(272),self.C.c_uint(state))
    def close(self):self.lib.wl_display_disconnect(self.display)

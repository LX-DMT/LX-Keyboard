"""Rounded option menu for setup fields, retaining native editable text input."""
import ctypes as C
from ctypes import wintypes as W
import tkinter as tk
from tkinter import ttk, font as tkfont
from ui_widgets import INK, GREEN, SOFT, LINE, SlimScrollbar, font, scale, rounded_image, icon_image


def round_window(window, width, height, radius):
    """Clip the native popup, without color-key halos on antialiased edges."""
    user, gdi = C.WinDLL('user32', use_last_error=True), C.WinDLL('gdi32', use_last_error=True)
    gdi.CreateRoundRectRgn.argtypes = [C.c_int]*6
    gdi.CreateRoundRectRgn.restype = W.HANDLE
    gdi.DeleteObject.argtypes = [W.HANDLE]
    user.SetWindowRgn.argtypes = [W.HWND, W.HANDLE, W.BOOL]
    region = gdi.CreateRoundRectRgn(0,0,width+1,height+1,round(2*radius),round(2*radius))
    if not region: raise C.WinError(C.get_last_error())
    hwnd = int(str(window.tk.call('wm','frame',window)),0)
    if not user.SetWindowRgn(hwnd, region, True):
        gdi.DeleteObject(region)
        raise C.WinError(C.get_last_error())
    # Windows owns the region after successful SetWindowRgn.


class Dropdown(ttk.Combobox):
    def __init__(self, parent, **kw):
        self.option_label = kw.pop('option_label', lambda value: value)
        super().__init__(parent, exportselection=False, **kw)
        self.popup = None
        self.bind('<ButtonPress-1>', self._press)
        for key in ('Down', 'Alt-Down', 'Alt-Up', 'F4'):
            self.bind('<'+key+'>', self.open_menu)
        self.bind('<Escape>', self._escape)
        self.bind('<Unmap>', lambda e: self.close_menu(False))
        self.owner = self.winfo_toplevel()
        self.owner_bind = self.owner.bind('<Configure>', lambda e: self.close_menu(False) if e.widget is self.owner else None, add='+')
        self.bind('<Destroy>', self._destroy)

    def _destroy(self, event):
        if event.widget is not self: return
        self.close_menu(False)
        if self.owner.winfo_exists(): self.owner.unbind('<Configure>', self.owner_bind)

    def configure(self, cnf=None, **kw):
        if cnf: kw.update(cnf)
        # A hotplug refresh or transaction must not leave stale choices open.
        if 'values' in kw or kw.get('state') == 'disabled':
            self.close_menu(False)
        return super().configure(**kw)
    config = configure

    def _press(self, event):
        if self.instate(['disabled']): return 'break'
        if self.instate(['readonly']) or 'downarrow' in self.identify(event.x, event.y):
            self.focus_set()
            return self.open_menu()

    def _escape(self, event=None):
        if self.popup:
            self.close_menu()
            return 'break'

    def open_menu(self, event=None):
        if self.popup:
            self.close_menu()
            return 'break'
        if self.instate(['disabled']) or not self['values']: return 'break'
        self.focus_set()
        self.options = list(self['values'])
        self.active = self.options.index(self.get()) if self.get() in self.options else 0
        self.visible = min(8, len(self.options))
        self.offset = max(0, min(self.active, len(self.options)-self.visible))
        self.s = scale(self)
        self.pad, self.row_height = round(6*self.s), round(36*self.s)
        self.previous_grab = self.grab_current()
        popup = self.popup = tk.Toplevel(self)
        popup.withdraw(); popup.overrideredirect(True)
        popup.transient(self.winfo_toplevel())
        popup.configure(bg='white')
        anchor = getattr(self, 'field_surface', self)
        width = anchor.winfo_width()
        height = self.visible*self.row_height + 2*self.pad
        x = anchor.winfo_rootx()
        y = anchor.winfo_rooty()+anchor.winfo_height()+round(4*self.s)
        if y+height > self.winfo_screenheight(): y = anchor.winfo_rooty()-height-round(4*self.s)
        popup.geometry(f'{width}x{height}+{x}+{max(0,y)}')
        self.menu = tk.Canvas(popup, bg='white', highlightthickness=0, bd=0,
                              width=width, height=height, takefocus=1)
        self.menu.pack(fill='both', expand=True)
        self.scrollbar = SlimScrollbar(self.menu, command=self._scroll)
        self.scrollbar.configure(bg='white', takefocus=0)
        if len(self.options)>self.visible:
            self.scrollbar.place(x=width-round(18*self.s), y=self.pad,
                                 width=round(12*self.s), height=height-2*self.pad)
        self.menu.bind('<Configure>', lambda e: self._draw())
        self.menu.bind('<Motion>', self._hover)
        popup.bind('<ButtonPress-1>', self._choose_click)
        popup.bind('<MouseWheel>', self._wheel)
        popup.bind('<Down>', lambda e: self._move(1))
        popup.bind('<Up>', lambda e: self._move(-1))
        popup.bind('<Home>', lambda e: self._move(-len(self.options)))
        popup.bind('<End>', lambda e: self._move(len(self.options)))
        popup.bind('<Return>', lambda e: self._choose())
        popup.bind('<Escape>', self._escape)
        popup.bind('<Tab>', lambda e: self._tab(False))
        popup.bind('<Shift-Tab>', lambda e: self._tab(True))
        popup.bind('<FocusOut>', lambda e: self.after_idle(self._focus_lost))
        popup.update_idletasks(); round_window(popup, width, height, 10*self.s)
        popup.deiconify(); popup.lift(); popup.grab_set(); self.menu.focus_set()
        return 'break'

    def _focus_lost(self):
        if self.popup:
            focused = self.focus_get()
            if focused is None or focused.winfo_toplevel() != self.popup: self.close_menu(False)

    def close_menu(self, restore_focus=True):
        popup = getattr(self, 'popup', None)
        if not popup: return
        self.popup = None
        if not popup.winfo_exists(): return
        if popup.grab_current() == popup:
            popup.grab_release()
            previous = self.previous_grab
            if previous is not None and previous.winfo_exists(): previous.grab_set()
        if self.winfo_exists():
            focused = self.focus_get()
            if restore_focus: self.focus_set()
            elif focused is not None and focused.winfo_toplevel() == popup:
                self.winfo_toplevel().focus_set()
        popup.destroy()

    def _tab(self, backward):
        target = self.tk_focusPrev() if backward else self.tk_focusNext()
        target.focus_set(); self.close_menu(False)
        return 'break'

    def _move(self, delta):
        self.active = min(len(self.options)-1, max(0, self.active+delta))
        self.offset = min(self.offset, self.active)
        self.offset = max(self.offset, self.active-self.visible+1)
        self._draw()
        return 'break'

    def _scroll(self, action, amount, unit=None):
        if action == 'moveto': offset = round(float(amount)*len(self.options))
        else: offset = self.offset+int(amount)*(self.visible if unit=='pages' else 1)
        self.offset = max(0, min(len(self.options)-self.visible, offset))
        self._draw()

    def _wheel(self, event):
        self._scroll('scroll', -1 if event.delta>0 else 1, 'units')
        return 'break'

    def _hover(self, event):
        index = (event.y-self.pad)//self.row_height + self.offset
        if self.pad <= event.y < self.pad+self.visible*self.row_height and index != self.active:
            self.active = index; self._draw()

    def _choose_click(self, event):
        if event.widget == self.scrollbar: return
        x, y = event.x_root-self.menu.winfo_rootx(), event.y_root-self.menu.winfo_rooty()
        if self.pad <= x < self.menu.winfo_width()-self.pad and self.pad <= y < self.pad+self.visible*self.row_height:
            self.active = (y-self.pad)//self.row_height + self.offset
            return self._choose()
        self.close_menu()
        return 'break'

    def _choose(self):
        value = self.options[self.active]
        self.set(value); self.selection_clear(); self.close_menu()
        self.event_generate('<<ComboboxSelected>>', when='tail')
        return 'break'

    def _draw(self):
        if not self.popup: return
        canvas = self.menu; w, h = canvas.winfo_width(), canvas.winfo_height()
        if w<2 or h<2: return
        canvas.delete('all'); self.images = []
        def image(im, x, y):
            self.images.append(im); canvas.create_image(x, y, image=im, anchor='nw')
        image(rounded_image(canvas,w,h,10*self.s,'white',LINE),0,0)
        right = round((22 if len(self.options)>self.visible else 6)*self.s)
        text_font = tkfont.Font(canvas, font=font(canvas,12))
        for row, index in enumerate(range(self.offset,self.offset+self.visible)):
            y = self.pad+row*self.row_height
            selected = self.options[index] == self.get()
            if index == self.active or selected:
                image(rounded_image(canvas,w-self.pad-right,self.row_height,7*self.s,SOFT),self.pad,y)
            text = self.option_label(self.options[index]); available = w-right-round(52*self.s)
            if text_font.measure(text)>available:
                while text and text_font.measure(text+'…')>available: text=text[:-1]
                text += '…'
            canvas.create_text(round(16*self.s),y+self.row_height/2,anchor='w',text=text,
                               fill=GREEN if selected else INK,font=font(canvas,12))
            if selected: image(icon_image(canvas,'check',16,GREEN),w-right-round(25*self.s),y+(self.row_height-round(16*self.s))/2)
        self.scrollbar.set(self.offset/len(self.options),(self.offset+self.visible)/len(self.options))

"""DPI-scaled, antialiased primitives matching the approved desktop mockup."""
import tkinter as tk
from tkinter import font as tkfont
from PIL import Image, ImageDraw, ImageTk

BG = '#f6f7f4'
WHITE = '#fcfdfa'
INK = '#23312f'
MUTED = '#7c8783'
GREEN = '#2c7260'
SOFT = '#eaf2ed'
LINE = '#e7ebe7'


def scale(widget):
    return widget.winfo_fpixels('1i') / 96


def font(widget, size=12, bold=False):
    # Tk's Segoe UI fallback uses SimSun for CJK. Select the installed Chinese
    # family explicitly so regular text stays sans serif like the CSS preview.
    return ('微软雅黑', -round(size * scale(widget)), 'bold' if bold else 'normal')


def rounded_image(widget, w, h, radius, fill, border=None):
    factor = 3
    image = Image.new('RGBA', (max(1, w)*factor, max(1, h)*factor))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((1, 1, w*factor-2, h*factor-2), radius=radius*factor,
                           fill=fill, outline=border, width=max(1, round(scale(widget)*factor)))
    return ImageTk.PhotoImage(image.resize((w, h), Image.Resampling.LANCZOS), master=widget)


def icon_image(widget, name, size=22, color=INK):
    """Same 24-unit line geometry as the preview SVGs; no platform font glyphs."""
    pixels = max(1, round(size*scale(widget)))
    factor = 8 if name=='mic' else 4
    im = Image.new('RGBA', (pixels*factor, pixels*factor))
    d = ImageDraw.Draw(im); unit = pixels*factor/24; stroke = max(1, round(1.65*unit))
    def line(points):
        pts=[(x*unit,y*unit) for x,y in points]
        d.line(pts,fill=color,width=stroke,joint='curve')
        for x,y in (pts[0],pts[-1]):
            r=stroke/2;d.ellipse((x-r,y-r,x+r,y+r),fill=color)
    def rect(box, radius=0):
        d.rounded_rectangle(tuple(v*unit for v in box),radius=radius*unit,outline=color,width=stroke)
    def dot(x,y,r):
        d.ellipse(((x-r)*unit,(y-r)*unit,(x+r)*unit,(y+r)*unit),fill=color)
    if name=='usb':
        for p in [[(12,20),(12,3)],[(9,6),(12,3),(15,6)],[(12,14),(6,10),(6,7)],[(12,17),(18,13),(18,8)]]:line(p)
        dot(12,20,1.5);rect((16.5,5.5,19.5,8.5))
    elif name=='ble':line([(6,7),(18,17),(12,22),(12,2),(18,7),(6,17)])
    elif name=='receiver':
        rect((7,9,17,21),2);line([(9,9),(9,3),(15,3),(15,9)])
        for p in [[(11,5),(11,6)],[(13,5),(13,6)],[(10,18),(14,18)]]:line(p)
    elif name=='wifi':
        # Concentric arcs with rounded ends, centered on a clear signal dot.
        from math import cos,sin,pi
        for radius in (7,14):
            line([(12+radius*cos((230+i*2)*pi/180),20+radius*sin((230+i*2)*pi/180)) for i in range(41)])
        dot(12,20,1.45)
    elif name=='pc':
        rect((2,3,22,17),2);line([(12,17),(12,21)]);line([(7,21),(17,21)])
    elif name=='keyboard':
        rect((2,5,22,19),3)
        for y in (9,12):
            for x in (6,10,14,18):line([(x,y),(x+.7,y)])
        line([(7,15),(17,15)])
    elif name=='mic':
        from math import cos,sin,pi
        def arc(cx,cy,r,start,end):
            return [(cx+r*cos(a*pi/180),cy+r*sin(a*pi/180)) for a in range(start,end+1,2)]
        # Continuous centerline strokes avoid seams between the cup and arms.
        capsule=arc(12,5,3,180,360)+arc(12,12,3,0,180)+[(9,5)]
        line(capsule)
        line([(5.5,10)]+list(reversed(arc(12,11.5,6.5,0,180)))+[(18.5,10)])
        line([(12,18),(12,22)]);line([(9,22),(15,22)])
    elif name=='sun':
        from math import cos,sin,pi
        d.ellipse((7*unit,7*unit,17*unit,17*unit),outline=color,width=stroke)
        for i in range(8):
            angle=i*pi/4
            line([(12+8*cos(angle),12+8*sin(angle)),(12+10*cos(angle),12+10*sin(angle))])
    elif name=='settings':
        for y,x in ((6,9),(12,16),(18,8)):line([(4,y),(20,y)]);dot(x,y,2)
    elif name=='battery':
        rect((2,6,20,18),2)
        for x in (6,10,14,22):line([(x,10),(x,14)])
    elif name=='plus':line([(12,5),(12,19)]);line([(5,12),(19,12)])
    elif name=='down':line([(7,10),(12,15),(17,10)])
    elif name=='arrow':line([(9,5),(16,12),(9,19)])
    elif name=='check':line([(6,12),(10,16),(18,6)])
    return ImageTk.PhotoImage(im.resize((pixels,pixels),Image.Resampling.LANCZOS),master=widget)


class Surface(tk.Frame):
    def __init__(self,parent,fill='white',radius=12,padding=(24,20),border=LINE,**kw):
        super().__init__(parent,bg=parent.cget('background'),**kw)
        self.fill,self.radius,self.border=fill,radius,border
        self.canvas=tk.Canvas(self,bg=self.cget('bg'),highlightthickness=0,bd=0)
        self.canvas.place(x=0,y=0,relwidth=1,relheight=1)
        self.body=tk.Frame(self,bg=fill)
        s=scale(self);self.body.pack(fill='both',expand=True,padx=max(1,round(padding[0]*s)),pady=max(1,round(padding[1]*s)))
        self.canvas.bind('<Configure>',self.draw)

    def draw(self,event):
        if event.width<2 or event.height<2:return
        self.image=rounded_image(self,event.width,event.height,self.radius*scale(self),self.fill,self.border)
        self.canvas.delete('all');self.canvas.create_image(0,0,image=self.image,anchor='nw')


class Button(tk.Canvas):
    """Focusable button with Tk-compatible invoke/state/text updates."""
    def __init__(self,parent,text='',command=None,icon=None,kind='button',width=None,height=38,
                 variable=None,value=None,**kw):
        self.s=scale(parent);self.text=text;self.command=command;self.icon=icon;self.kind=kind
        self.variable,self.value=variable,value;self.selected=False;self.disabled=False;self.hover=False
        self.status_text=''
        self.on=False;self.foreground=INK
        width=width or (tkfont.Font(parent,font=font(parent)).measure(text)/self.s+32+(26 if icon else 0))
        super().__init__(parent,bg=parent.cget('background'),width=round(width*self.s),height=round(height*self.s),
                         highlightthickness=0,bd=0,takefocus=1,cursor='hand2',**kw)
        self.bind('<Configure>',lambda e:self.draw())
        self.bind('<Enter>',lambda e:self._hover(True));self.bind('<Leave>',lambda e:self._hover(False))
        self.bind('<Button-1>',lambda e:(self.focus_set(),self.invoke()))
        self.bind('<space>',lambda e:self.invoke());self.bind('<Return>',lambda e:self.invoke())
        self.bind('<FocusIn>',lambda e:self.draw());self.bind('<FocusOut>',lambda e:self.draw())
        if variable:self.trace=variable.trace_add('write',lambda *a:self.draw())
        self.bind('<Destroy>',self._destroy,add='+')

    def _destroy(self,event):
        if event.widget is self and self.variable:self.variable.trace_remove('write',self.trace)

    def _hover(self,value):self.hover=value;self.draw()

    def invoke(self):
        if self.disabled:return
        if self.variable:self.variable.set(self.value)
        if self.command:return self.command()

    def configure(self,cnf=None,**kw):
        if cnf:kw.update(cnf)
        changed=False
        for key in ('text','selected','on','status_text'):
            if key in kw:
                value=kw.pop(key);changed|=getattr(self,key)!=value;setattr(self,key,value)
        if 'state' in kw:
            disabled=kw.pop('state')=='disabled';changed|=self.disabled!=disabled;self.disabled=disabled
        if 'fg' in kw:
            value=kw.pop('fg');changed|=self.foreground!=value;self.foreground=value
        changed|=bool(kw)
        result=super().configure(**kw) if kw else None
        if changed and hasattr(self,'s'):self.draw()
        return result
    config=configure

    def __setitem__(self,key,value):self.configure(**{key:value})
    def __getitem__(self,key):
        if key=='state':return 'disabled' if self.disabled else 'normal'
        if key=='text':return self.text
        return super().__getitem__(key)

    def draw(self):
        if not self.winfo_exists():return
        w,h=self.winfo_width(),self.winfo_height()
        if w<2 or h<2:return
        selected=self.variable.get()==self.value if self.variable else self.selected
        color=GREEN if selected or self.kind=='link' else INK
        fill=SOFT if selected else 'white';border='#75a48f' if selected else '#dce3dc'
        if self.kind in ('nav','link','toggle'):
            fill=SOFT if selected else self.cget('bg');border=None
        if self.kind=='primary':fill=GREEN;color='white';border=GREEN
        if self.hover and not self.disabled and not selected:fill=SOFT if self.kind!='primary' else '#235c4d'
        if self.kind=='nav' and not selected:color=MUTED
        if self.disabled:
            color='#a3aaa5';fill='#9cbab0' if self.kind=='primary' else '#f1f3f0'
            if self.kind=='mode':border='#e0e4df'
        if self.kind=='primary' and self.disabled:color='white'
        if self.focus_get() is self:border='#75a48f'
        if self.kind=='toggle':fill=self.cget('bg');border=None
        self.delete('all');self.images=[]
        if fill!=self.cget('bg') or border:
            image=rounded_image(self,w,h,(9 if self.kind=='mode' else 8)*self.s,fill,border)
            self.images.append(image);self.create_image(0,0,image=image,anchor='nw')
        def ico(name,x,y,size=19):
            im=icon_image(self,name,size,color);self.images.append(im);self.create_image(x,y,image=im)
        if self.kind=='mode':
            ico(self.icon,w/2,h/2-(21 if self.status_text else 13)*self.s,22)
            self.create_text(w/2,h/2+(6 if self.status_text else 17)*self.s,text=self.text,fill=color,font=font(self,12))
            if self.status_text:self.create_text(w/2,h/2+27*self.s,text=self.status_text,fill=color if self.disabled else MUTED,font=font(self,10))
            if selected and not self.disabled:ico('check',w-12*self.s,12*self.s,11)
        elif self.kind=='nav':
            ico(self.icon,24*self.s,h/2,17)
            self.create_text(45*self.s,h/2,text=self.text,fill=color,anchor='w',font=font(self,13,selected))
        elif self.kind=='toggle':
            self.create_text(w-41*self.s,h/2,text=self.text,anchor='e',fill=GREEN if self.focus_get() is self else MUTED,font=font(self,11))
            track=rounded_image(self,round(32*self.s),round(18*self.s),9*self.s,GREEN if self.on else '#cfd6cf')
            self.images.append(track);self.create_image(w-32*self.s,h/2,image=track,anchor='w')
            x=w-(9 if self.on else 23)*self.s;r=6*self.s
            self.create_oval(x-r,h/2-r,x+r,h/2+r,fill='white',outline='')
        else:
            tw=tkfont.Font(self,font=font(self)).measure(self.text)
            start=0 if self.kind=='link' else (w-tw-(26*self.s if self.icon else 0))/2
            if self.icon=='down':
                self.create_text(12*self.s,h/2,text=self.text,anchor='w',fill=color,font=font(self))
                ico('down',w-15*self.s,h/2,13)
            else:
                if self.icon:ico(self.icon,start+9*self.s,h/2,17);start+=26*self.s
                self.create_text(start,h/2,text=self.text,anchor='w',fill=color,font=font(self))


class Badge(tk.Canvas):
    def __init__(self,parent,text,positive=False):
        s=scale(parent);self.text=text;self.positive=positive
        width=tkfont.Font(parent,font=font(parent,11)).measure(text)+round(20*s)
        super().__init__(parent,bg=parent.cget('background'),width=width,height=round(27*s),highlightthickness=0,bd=0)
        self.bind('<Configure>',lambda e:self.draw())
    def set(self,text):
        if text==self.text:return
        self.text=text
        self.configure(width=tkfont.Font(self,font=font(self,11)).measure(text)+round(20*scale(self)))
        self.draw()
    def draw(self):
        w,h=self.winfo_width(),self.winfo_height()
        self.image=rounded_image(self,w,h,h/2,'#edf5ee' if self.positive else '#f0f3f0')
        self.delete('all');self.create_image(0,0,image=self.image,anchor='nw')
        self.create_text(w/2,h/2,text=self.text,fill='#487651' if self.positive else MUTED,font=font(self,11))


class SlimScrollbar(tk.Canvas):
    """Quiet overlay thumb: no arrows, bevel or permanent track."""
    def __init__(self,parent,command):
        self.s=scale(parent);self.command=command
        self.first,self.last=0.,1.;self.hover=False;self.drag_offset=None
        super().__init__(parent,bg=parent.cget('background'),width=round(12*self.s),
                         highlightthickness=0,bd=0,takefocus=1)
        self.bind('<Configure>',lambda e:self.draw())
        self.bind('<Enter>',lambda e:self.highlight(True))
        self.bind('<Leave>',lambda e:self.highlight(False))
        self.bind('<FocusIn>',lambda e:self.draw());self.bind('<FocusOut>',lambda e:self.draw())
        self.bind('<Button-1>',self.press);self.bind('<B1-Motion>',self.drag)
        self.bind('<ButtonRelease-1>',self.release)
        for key,direction,unit in [('Up',-1,'units'),('Down',1,'units'),('Prior',-1,'pages'),('Next',1,'pages')]:
            self.bind('<'+key+'>',lambda e,d=direction,u=unit:self.command('scroll',d,u))
        self.bind('<Home>',lambda e:self.command('moveto',0))
        self.bind('<End>',lambda e:self.command('moveto',1))

    def set(self,first,last):
        self.first,self.last=float(first),float(last);self.draw()

    def geometry(self):
        height=self.winfo_height();span=max(0.,min(1.,self.last-self.first))
        length=min(height,max(round(28*self.s),height*span))
        travel=max(0,height-length)
        top=travel*self.first/max(1e-9,1-span)
        return max(0,min(travel,top)),length,travel,span

    def highlight(self,value):self.hover=value;self.draw()

    def draw(self):
        self.delete('all')
        if self.last-self.first>=1 or self.winfo_height()<2:return
        top,length,_,_=self.geometry()
        active=self.hover or self.drag_offset is not None or self.focus_get() is self
        width=max(2,round((8 if active else 6)*self.s))
        self.thumb=rounded_image(self,width,max(1,round(length)),width/2,'#8fa99c' if active else '#c4cfc7')
        self.create_image(self.winfo_width()/2,top,image=self.thumb,anchor='n')

    def press(self,event):
        self.focus_set();top,length,_,_=self.geometry()
        if top<=event.y<=top+length:self.drag_offset=event.y-top
        else:self.command('scroll',-1 if event.y<top else 1,'pages')
        self.draw()

    def drag(self,event):
        if self.drag_offset is None:return
        _,_,travel,span=self.geometry()
        if travel:self.command('moveto',max(0,min(1,(event.y-self.drag_offset)/travel))*(1-span))

    def release(self,event):self.drag_offset=None;self.draw()


class Progress(tk.Canvas):
    def __init__(self,parent):
        self.value=0
        super().__init__(parent,bg=parent.cget('background'),height=round(4*scale(parent)),highlightthickness=0,bd=0)
        self.bind('<Configure>',lambda e:self.draw())
    def __setitem__(self,key,value):
        if key=='value':self.value=value;self.draw()
        else:super().__setitem__(key,value)
    def draw(self):
        w,h=self.winfo_width(),self.winfo_height()
        if w<2 or h<1:return
        self.image=rounded_image(self,w,h,h/2,'#eff2ed');self.delete('all');self.create_image(0,0,image=self.image,anchor='nw')
        length=round(w*max(0,min(100,self.value))/100)
        if length:
            self.bar=rounded_image(self,length,h,h/2,'#94b5a1');self.create_image(0,0,image=self.bar,anchor='nw')

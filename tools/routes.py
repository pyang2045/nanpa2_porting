"""Scripted-input routes for NANPA2_INPUT (run with NANPA2_INPUT_CLOCK=frames, NANPA2_HEADLESS=1
and a scratch NANPA2_SAVE_DIR). usage: routes.py ROUTE [X,Y[,HOLD_MS] ...] > input.txt

save40  needs FLAG0 naming open1.mes with flag 2010 = 0 (low nibble of byte 0x40d) and FLAG6:
        title Data Load, slot 0 -> OPEN1 -> 40.MES, then bed, sleep, write diary book 1 (FLAG1).
load40, to41, town, cafe, town2  load book 1 from the title (the Main item is shown by then)
        and go on from 40.MES; town2 takes town-map walks (X,Y,HOLD_MS) or clicks (X,Y).
See NOTES.md 2026-11-04 (engine stage 4)."""
import sys


class R:
    def __init__(self):
        self.ev = []
        self.t = 0

    def sync(self, name):
        self.ev.append('sync,0,' + name)
        self.t = 0

    def at(self, kind, t, x=None, y=None):
        self.t = t
        self.ev.append(f'{kind},{t}' + (f',{x},{y}' if x is not None else ''))

    def adv(self, n, step=900, shots=False, x=320, y=430):
        for _ in range(n):
            self.t += step
            if shots:
                self.ev.append(f'shot,{self.t - 100}')
            self.ev.append(f'click,{self.t},{x},{y}')

    def pick(self, x, y, gap=1500, shot=False):
        self.t += gap
        self.ev.append(f'move,{self.t},{x},{y}')
        if shot:
            self.ev.append(f'shot,{self.t + 400}')
        self.t += 600
        self.ev.append(f'click,{self.t},{x},{y}')

    def rclick(self, x, y, gap=1500):
        self.t += gap
        self.ev.append(f'move,{self.t},{x},{y}')
        self.t += 300
        self.ev.append(f'rclick,{self.t},{x},{y}')

    def shot(self, gap=500):
        self.t += gap
        self.ev.append(f'shot,{self.t}')

    def quit(self, gap=1000):
        self.t += gap
        self.ev.append(f'quit,{self.t}')

    def out(self):
        return ';'.join(self.ev)


def title_load(r, item_y, book_x, book_y=170):
    r.sync('start1.mes')
    r.at('move', 6000, 320, item_y)
    r.at('click', 6500, 320, item_y)
    r.at('move', 9000, book_x, book_y)
    r.at('click', 9500, book_x, book_y)


def open1_to_40(r):
    r.sync('open1.mes')
    r.adv(39, 1000)
    for k in range(3):
        r.pick(197, 124, gap=1000)
        r.adv(25, 900)
    r.sync('40.mes')


route = sys.argv[1]
MOVES = [tuple(map(int, p.split(','))) for p in sys.argv[2:]]
r = R()
if route == 'save40':
    # synthetic OPEN1 save (slot 0) -> 40.MES -> bed -> sleep 1 h -> write diary -> book 1
    title_load(r, 226, 440)
    open1_to_40(r)
    r.adv(20, 900)
    r.pick(150, 250)            # bed (hotspot 0x16)
    r.adv(1, 1500)
    r.pick(100, 57, gap=1500)   # 睡1小時
    r.adv(1, 1500)
    r.pick(100, 57, gap=1500)   # 寫日記
    r.pick(295, 82, gap=2500, shot=True)   # book 1 -> FLAG1
    r.adv(6, 1200, shots=True)
    r.quit()
elif route == 'load40':
    # title (Prologue / Main / Data Load) -> Data Load -> book 1 -> 40.MES, right-click
    title_load(r, 254, 200)
    r.sync('40.mes')
    r.adv(8, 1200, shots=True)
    r.rclick(150, 280)
    r.shot(700)
    r.pick(150, 57)             # 到客廳去
    r.adv(6, 1200, shots=True)
    r.quit()
elif route == 'to41':
    title_load(r, 254, 200)
    r.sync('40.mes')
    r.adv(8, 1200)
    r.rclick(150, 280)
    r.shot(700)
    r.pick(150, 57)             # 到客廳去
    r.adv(3, 1200)              # 知道了，去客廳吧 waits for a click before 41.MES loads
    r.sync('41.mes')
    r.adv(30, 1200, shots=True)
    r.quit()
elif route == 'town':
    title_load(r, 254, 200)
    r.sync('40.mes')
    r.adv(8, 1200)
    r.rclick(150, 280)
    r.shot(700)
    r.pick(150, 57)             # 到客廳去
    r.adv(3, 1200)
    r.sync('41.mes')
    r.adv(8, 1200)
    r.rclick(500, 280)
    r.shot(700)
    r.pick(150, 111, shot=True) # 4th item: go out
    r.adv(4, 1200, shots=True)
    r.sync('town.mes')
    r.adv(3, 1200)
    for x, y, *h in MOVES:
        if h:   # hold the button for h[0] ms
            r.at('move', r.t + 800, x, y)
            r.at('down', r.t + 200, x, y)
            t0 = r.t
            while r.t < t0 + h[0]:
                r.shot(500)
            r.at('up', r.t + 50, x, y)
            r.shot(500)
        else:
            r.pick(x, y, gap=800)
            for _ in range(6):
                r.shot(500)
    r.quit()
elif route == 'cafe':
    title_load(r, 254, 200)
    r.sync('40.mes')
    r.adv(8, 1200)
    r.rclick(150, 280)
    r.shot(700)
    r.pick(150, 57)             # 到客廳去
    r.adv(3, 1200)
    r.sync('41.mes')
    r.adv(8, 1200)
    r.rclick(500, 280)
    r.shot(700)
    r.pick(150, 111)            # 4th item: go out
    r.adv(4, 1200)
    r.sync('town.mes')
    r.adv(3, 1200)
    for _ in range(8):          # click the house next to the player until 39.MES loads
        r.pick(300, 130, gap=600)
    r.sync('39.mes')
    r.adv(40, 1000, shots=False)
    r.shot()
    r.rclick(500, 280)
    r.shot(700)
    r.pick(150, 75)             # 進咖啡廳
    for _ in range(4):
        r.adv(1, 1000)
    r.sync('63a.mes')
    r.adv(30, 1000, shots=True)
    r.quit()
elif route == 'town2':
    title_load(r, 254, 200)
    r.sync('40.mes')
    r.adv(8, 1200)
    r.rclick(150, 280)
    r.shot(700)
    r.pick(150, 57)             # 到客廳去
    r.adv(3, 1200)
    r.sync('41.mes')
    r.adv(8, 1200)
    r.rclick(500, 280)
    r.shot(700)
    r.pick(150, 111)            # 4th item: go out
    r.adv(4, 1200)
    r.sync('town.mes')
    r.adv(3, 1200)
    for _ in range(8):
        r.pick(300, 130, gap=600)
    r.sync('39.mes')
    r.adv(40, 1000)
    r.rclick(500, 280)
    r.shot(700)
    r.pick(150, 111)            # 到其他地方
    r.adv(3, 1000)
    r.sync('town.mes')
    r.adv(2, 1000)
    r.shot()
    for x, y, *h in MOVES:
        if h:
            r.at('move', r.t + 800, x, y)
            r.at('down', r.t + 200, x, y)
            r.at('up', r.t + h[0], x, y)
            r.shot(300)
        else:
            r.pick(x, y, gap=800)
            r.shot(400)
    r.adv(10, 1000, shots=True)
    r.quit()
print(r.out())

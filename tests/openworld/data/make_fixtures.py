"""Writes the World Remix test fixtures: two small synthetic places (ESRI ASCII elevation in degrees and Overpass JSON).

coast: sea to the west, land rising east, an east-west ridge with a primary road on its crest, a river in the valley to
the north draining to the sea, coastline, a north-south secondary road, a town area and a cafe.
inland: rolling hills with a trunk road east-west and a small grid town.
Run from this folder: python make_fixtures.py
"""
import json
import math

def asc(path, w, h, x0, y0, d, f):
    with open(path, "w", newline="\n") as o:
        o.write(f"ncols {w}\nnrows {h}\nxllcorner {x0 - d / 2:.6f}\nyllcorner {y0 - d / 2:.6f}\ncellsize {d}\nNODATA_value -9999\n")
        for r in range(h):
            row = []
            for c in range(w):
                u, v = c / (w - 1), 1 - r / (h - 1)  # u east, v north, 0..1
                row.append(f"{f(u, v):.1f}")
            o.write(" ".join(row) + "\n")

def coast_height(u, v):
    if u < 0.3:
        return -20.0 - (0.3 - u) * 100.0
    land = (u - 0.3) / 0.7
    base = 5.0 + land * 300.0
    ridge = 180.0 * math.exp(-((v - 0.5) / 0.08) ** 2)
    valley = -60.0 * math.exp(-((v - 0.75) / 0.05) ** 2)
    return base + ridge * min(1.0, land * 4) + valley * min(1.0, land * 3)

def inland_height(u, v):
    return 200.0 + 60.0 * math.sin(u * 6.0) * math.cos(v * 5.0) + 40.0 * u

class Osm:
    def __init__(self, x0, y0, span):
        self.x0, self.y0, self.span, self.els, self.next = x0, y0, span, [], 1000

    def ll(self, u, v):
        return self.y0 + v * self.span, self.x0 + u * self.span

    def node(self, u, v, tags=None):
        self.next += 1
        lat, lon = self.ll(u, v)
        e = {"type": "node", "id": self.next, "lat": round(lat, 7), "lon": round(lon, 7)}
        if tags:
            e["tags"] = tags
        self.els.append(e)
        return self.next

    def way(self, pts, tags, closed=False):
        ids = [self.node(u, v) for u, v in pts]
        if closed:
            ids.append(ids[0])
        self.next += 1
        self.els.append({"type": "way", "id": self.next, "nodes": ids, "tags": tags})
        return self.next

    def write(self, path):
        with open(path, "w", newline="\n") as o:
            json.dump({"version": 0.6, "elements": self.els}, o, indent=0)

D = 0.0005
N = 121
X0, Y0 = -107.0, 24.0
asc("coast.asc", N, N, X0, Y0, D, coast_height)
o = Osm(X0, Y0, D * (N - 1))
o.way([(0.31 + 0.69 * k / 40, 0.5) for k in range(41)], {"highway": "primary", "name": "Ridge Road"})
o.way([(1.0 - 0.75 * k / 40, 0.75) for k in range(41)], {"waterway": "river", "name": "Rio Uno"})
o.way([(0.3, 1.0 - k / 20) for k in range(21)], {"natural": "coastline"})
o.way([(0.7, 0.05 + 0.4 * k / 10) for k in range(11)] + [(0.7, 0.55 + 0.15 * k / 5) for k in range(6)], {"highway": "secondary"})
o.way([(0.55, 0.2), (0.65, 0.2), (0.65, 0.3), (0.55, 0.3)], {"landuse": "residential"}, closed=True)
o.node(0.6, 0.25, {"amenity": "cafe", "name": "Cafe Real"})
o.write("coast.json")

N2 = 81
X1, Y1 = -107.4, 25.0
asc("inland.asc", N2, N2, X1, Y1, D, inland_height)
o = Osm(X1, Y1, D * (N2 - 1))
o.way([(0.02 + 0.96 * k / 30, 0.5) for k in range(31)], {"highway": "trunk", "name": "Highway"})
for k in range(4):
    o.way([(0.35 + 0.1 * k, 0.3), (0.35 + 0.1 * k, 0.7)], {"highway": "residential"})
    o.way([(0.3, 0.35 + 0.1 * k), (0.7, 0.35 + 0.1 * k)], {"highway": "residential"})
o.way([(0.3, 0.3), (0.7, 0.3), (0.7, 0.7), (0.3, 0.7)], {"landuse": "residential"}, closed=True)
o.node(0.5, 0.5, {"amenity": "townhall", "name": "Palacio"})
o.write("inland.json")

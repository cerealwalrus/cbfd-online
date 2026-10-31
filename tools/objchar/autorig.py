# Fits model 3B's skeleton (Conker's 28 bones: the order, parents and bind pose his animations need) to a mesh with no skeleton
# of its own, and binds each vertex to one bone (rigid, as the game's own characters are): for tools/objchar/obj_character.py.
#
# The mesh is measured, not guessed at:
#  - its middle is the median of its vertices (a staff or a strap on one side doesn't move it); its ends (hands, feet, the top of
#    its head, a tail's tip) are the points farther along its own surface from its chest than everything around them;
#  - each limb's joint is found by cutting the mesh across the limb, walking from its end in towards the chest: the cut's ring
#    stays the limb's size down the limb, and grows suddenly where the limb joins the body (the shoulder, the hip, a tail's root);
#  - the neck is the narrowest place under the head, above where the body widens into the shoulders;
#  - every vertex goes to the part (limb or body) whose cuts it is nearest to along the surface.
# Its joints are put where its own limbs are, and its arms turned about the shoulders to hang as 3B's do in his bind pose, so his
# animations (rotations from that pose) move them as they move his. Four legs: the back pair on 3B's legs, the head and tail on his
# head and tail; the front pair go with the body (on his arms they'd be lifted to hold the guns).
import heapq
import numpy as np

PAR = [255, 0, 1, 2, 3, 4, 4, 1, 7, 8, 9, 9, 1, 12, 12, 12, 0, 16, 17, 18, 0, 20, 21, 22, 0, 24, 25, 26]
ROOT, SPINE, HEAD = 0, 1, 12
ARM = {+1: (2, 3, 4, 5, 6), -1: (7, 8, 9, 10, 11)}      # shoulder, elbow, wrist, hand's end, thumb
LEG = {+1: (16, 17, 18, 19), -1: (20, 21, 22, 23)}      # hip, knee, ankle, toe
TAIL = (24, 25, 26, 27)


def slice_x(V, T, h):
    y = V[T][:, :, 1]
    lo, hi = y.min(1), y.max(1)
    sel = np.where((lo <= h) & (hi >= h) & (hi > lo))[0]
    segs = []
    for t in sel:
        P = V[T[t]]
        xs = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            ya, yb = P[a][1], P[b][1]
            if (ya - h) * (yb - h) <= 0 and ya != yb:
                xs.append(P[a][0] + (P[b][0] - P[a][0]) * (h - ya) / (yb - ya))
        if len(xs) >= 2:
            segs.append((min(xs), max(xs)))
    if not segs:
        return []
    segs.sort()
    out = [list(segs[0])]
    for a, b in segs[1:]:
        if a <= out[-1][1] + 1e-6:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def central(iv, tol):
    for a, b in iv:
        if a <= tol and b >= -tol:
            return a, b
    return None


def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); s = np.linalg.norm(v); c = float(np.dot(a, b))
    if s < 1e-9:
        return np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * ((1 - c) / s ** 2)


def canon_index(V, H):
    """Each vertex's first twin at its place (the mesh is split at its texture seams)."""
    key = {}
    canon = np.arange(len(V))
    for i, p in enumerate(V):
        k = tuple(np.round(p / (0.002 * H)).astype(int))
        if k in key:
            canon[i] = key[k]
        else:
            key[k] = i
    return canon


def graph(V, T, H, canon):
    """The mesh's edges; vertices at one place (seams) joined, and parts modelled apart (a glove, a hat) joined to the rest at
    their nearest points."""
    n = len(V)
    T = np.asarray(T)
    nb = [dict() for _ in range(n)]
    def link(i, j):
        if i != j:
            d = float(np.linalg.norm(V[i] - V[j]))
            nb[i][j] = d; nb[j][i] = d
    for t in T:
        link(t[0], t[1]); link(t[1], t[2]); link(t[2], t[0])
    for i in range(n):
        if canon[i] != i:
            link(i, int(canon[i]))
    comp = -np.ones(n, int)
    c = 0
    for s in range(n):
        if comp[s] >= 0:
            continue
        stack = [s]; comp[s] = c
        while stack:
            i = stack.pop()
            for j in nb[i]:
                if comp[j] < 0:
                    comp[j] = c; stack.append(j)
        c += 1
    graph.parts = comp
    if c > 1:
        sizes = np.bincount(comp)
        order = np.argsort(-sizes)
        main = np.zeros(n, bool)
        main[comp == order[0]] = True
        for k in order[1:]:
            ids = np.where(comp == k)[0]
            tri = T[np.all(main[T], axis=1)]
            if len(tri) == 0:
                continue
            A, B, C = V[tri[:, 0]], V[tri[:, 1]], V[tri[:, 2]]
            best = (np.inf, None, None)
            for i in ids:
                d, t = point_tri(V[i], A, B, C)
                if d < best[0]:
                    best = (d, i, t)
            d, i, t = best
            for j in tri[t]:
                link(int(i), int(j))
            main[ids] = True
    return nb


def point_tri(p, A, B, C):
    """The distance from p to the nearest of the triangles (A, B, C: their corners, one row each), and which one."""
    # (the nearest point on each: in the plane, else on an edge)
    AB, AC = B - A, C - A
    N = np.cross(AB, AC)
    nn = np.einsum('ij,ij->i', N, N)
    ok = nn > 1e-12
    Nn = np.where(ok[:, None], N / np.sqrt(np.where(ok, nn, 1.0))[:, None], 0.0)
    dpl = np.einsum('ij,ij->i', p - A, Nn)
    q = p - dpl[:, None] * Nn
    # (barycentric of q)
    v2 = q - A
    d00 = np.einsum('ij,ij->i', AB, AB); d01 = np.einsum('ij,ij->i', AB, AC); d11 = np.einsum('ij,ij->i', AC, AC)
    d20 = np.einsum('ij,ij->i', v2, AB); d21 = np.einsum('ij,ij->i', v2, AC)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    v = (d11 * d20 - d01 * d21) / den; w = (d00 * d21 - d01 * d20) / den
    inside = ok & (v >= 0) & (w >= 0) & (v + w <= 1)
    best = np.where(inside, np.abs(dpl), np.inf)
    def seg(P0, P1):
        D = P1 - P0
        L = np.einsum('ij,ij->i', D, D)
        t = np.clip(np.einsum('ij,ij->i', p - P0, D) / np.where(L < 1e-12, 1.0, L), 0.0, 1.0)
        return np.linalg.norm(p - (P0 + t[:, None] * D), axis=1)
    best = np.minimum(best, np.minimum(seg(A, B), np.minimum(seg(B, C), seg(C, A))))
    t = int(np.argmin(best))
    return float(best[t]), t


def dijkstra(nb, src):
    n = len(nb)
    dist = np.full(n, np.inf); prev = -np.ones(n, int)
    q = []
    for s in (src if isinstance(src, (list, tuple)) else [src]):
        dist[s] = 0.0
        q.append((0.0, s))
    heapq.heapify(q)
    while q:
        d, i = heapq.heappop(q)
        if d > dist[i]:
            continue
        for j, w in nb[i].items():
            nd = d + w
            if nd < dist[j]:
                dist[j] = nd; prev[j] = i
                heapq.heappush(q, (nd, j))
    return dist, prev


def path_to(prev, i):
    out = [i]
    while prev[out[-1]] >= 0:
        out.append(int(prev[out[-1]]))
    return out          # (from i back to the source)


def cut_ring(V, Tc, p, d):
    """The mesh (its triangles Tc, seams joined) cut by the plane through p across d: the piece of the cut nearest p, as its points
    and the vertices of the triangles it crosses."""
    s = (V - p) @ d
    neg = s[Tc] < 0
    cnt = neg.sum(1)
    idx = np.where((cnt > 0) & (cnt < 3))[0]
    if len(idx) == 0:
        return None, None
    parent = {}
    pts = {}
    tri_of = {}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for t in idx:
        tri = Tc[t]; ng = neg[t]
        ek = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            if ng[a] != ng[b]:
                i, j = int(tri[a]), int(tri[b])
                key = (i, j) if i < j else (j, i)
                if key not in pts:
                    sa, sb = s[key[0]], s[key[1]]
                    pts[key] = V[key[0]] + (V[key[1]] - V[key[0]]) * (sa / (sa - sb))
                    parent[key] = key
                ek.append(key)
        if len(ek) == 2:
            ra, rb = find(ek[0]), find(ek[1])
            if ra != rb:
                parent[ra] = rb
            tri_of.setdefault(ek[0], []).append(t)
    keys = list(pts)
    P = np.array([pts[k] for k in keys])
    near = keys[int(np.argmin(np.linalg.norm(P - p, axis=1)))]
    r0 = find(near)
    mine = [k for k in keys if find(k) == r0]
    verts = set()
    for k in mine:
        verts.update(k)
    return np.array([pts[k] for k in mine]), verts


def ring_profile(V, Tc, path, H, upto=None):
    """Down a limb from its end (path[0]) in towards the chest, every 1.5% of the height along the way: the size of the ring cut
    across the way there (the largest distance from the ring's middle), its middle, and the vertices it cuts through; each with the
    index of the way's vertex at or before it."""
    P = V[path]
    seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
    arc = np.concatenate([[0.0], np.cumsum(seg)])
    step = 0.015 * H
    out = []
    a = 0.0
    while a <= arc[-1] and (upto is None or a <= upto):
        k = min(len(path) - 2, int(np.searchsorted(arc, a, side='right')) - 1) if len(path) > 1 else 0
        if len(path) > 1 and seg[k] > 1e-9:
            p = P[k] + (P[k + 1] - P[k]) * ((a - arc[k]) / seg[k])
        else:
            p = P[k]
        def at(b):
            b = min(max(b, 0.0), arc[-1])
            j = min(len(path) - 2, int(np.searchsorted(arc, b, side='right')) - 1) if len(path) > 1 else 0
            return P[j] + (P[j + 1] - P[j]) * ((b - arc[j]) / seg[j]) if len(path) > 1 and seg[j] > 1e-9 else P[j]
        d = at(a - 0.03 * H) - at(a + 0.03 * H)
        nd = np.linalg.norm(d)
        if nd > 1e-6:
            ring, verts = cut_ring(V, Tc, p, d / nd)
            if ring is not None and len(ring) >= 2:
                c = ring.mean(0)
                out.append((k, float(a), float(np.linalg.norm(ring - c, axis=1).max()), c, verts))
        a += step
    return out


def find_joint(prof, H, min_len, upto, ratio=1.6, body_r=None):
    """Where the ring grows suddenly (the limb entering the body), at least min_len along and no farther in than upto: the places
    the rings just in from are much bigger than those just out from, in the order to try them: first those into rings of the body's
    size that stay that size (body_r: the torso's half width), then the rest from the body outwards (a gloved hand past a cane, a
    bulge, a forearm brushing the hip come after the shoulder)."""
    arcs = np.array([p[1] for p in prof]); rs = np.array([p[2] for p in prof])
    jumps = []
    for m in range(len(prof)):
        a = arcs[m]
        if a < min_len or a > upto:
            continue
        before = rs[(arcs >= max(0.02 * H, a - 0.12 * H)) & (arcs < a)]
        after = rs[(arcs >= a) & (arcs <= a + 0.05 * H)]
        if len(before) < 2 or len(after) < 2:
            continue
        b = float(np.median(before)); f = float(np.median(after))
        if f > ratio * b and f - b > 0.035 * H:
            # (a run of places over one jump: its first)
            if not jumps or a - arcs[jumps[-1]] > 0.05 * H:
                jumps.append(m)
    big = []
    if body_r is not None:
        for m in jumps:
            stay = rs[(arcs >= arcs[m]) & (arcs <= arcs[m] + 0.1 * H)]
            if float(np.median(stay)) >= 0.9 * body_r:
                big.append(m)
    return big + [m for m in reversed(jumps) if m not in big]


def neck_candidates(prof, top, H):
    """Down from the top (cuts (height, width of the middle piece)), the places the neck may be: just above each sudden widening
    (the shoulders, or ears or a hat joining the head), then the lowest place about as narrow as the narrowest below the head's
    widest before the body widens out (a big head's neck; a square head on sloping shoulders)."""
    out = []
    ws = [w for h, w in prof]
    for k in range(len(prof) - 2):
        h, w = prof[k]
        if top - h < 0.05 * H or w <= 0.04 * H:
            continue
        w2 = max(ws[k + 1:k + 3])
        if w2 > 1.5 * w and w2 - w > 0.05 * H:
            out.append(h)
    start = None
    for k in range(len(prof)):
        w = prof[k][1]
        if w <= 0.03 * H:
            continue
        nxt = ws[k + 1:k + 7]
        if nxt and w >= max(nxt) / 1.05:
            start = k
            break
    if start is not None:
        wmin = prof[start][1]
        cand = prof[start][0]
        found = None
        for h, w in prof[start:]:
            if w <= 0:
                continue
            if w > 1.5 * wmin and w - wmin > 0.05 * H:
                found = cand
                break
            wmin = min(wmin, w)
            if w <= 1.15 * wmin:
                cand = h
        if found is None and wmin < 0.85 * prof[start][1]:
            found = cand
        return out, found
    return out, None


def accessories(V, lab, parts, keep, body_like=(0,)):
    """Each piece modelled apart from the body (not the biggest, none of keep on it) whole to the part most of its vertices are
    nearest (straight-line) among the biggest piece's: a staff slung across the back stays on the back, not on the hand it
    happens to touch."""
    if parts is None:
        return lab
    sizes = np.bincount(parts)
    mainp = int(np.argmax(sizes))
    M = np.where(parts == mainp)[0]
    for k in range(len(sizes)):
        if k == mainp:
            continue
        ids = np.where(parts == k)[0]
        if len(ids) == 0 or any(i in keep for i in ids):
            continue
        d = np.linalg.norm(V[ids][:, None, :] - V[M][None, :, :], axis=2)
        near = lab[M[np.argmin(d, axis=1)]]
        cnt = np.bincount(near)
        top_ = int(cnt.argmax())
        # (on a limb only if nearly all of it is by that limb (a bracelet); else on the body or the head, whichever it's more by)
        if top_ not in body_like and cnt[top_] < 0.7 * len(ids):
            sub = [(cnt[b] if b < len(cnt) else 0, b) for b in body_like]
            top_ = max(sub)[1]
        lab[ids] = top_
    return lab


def bend_of(R, E, cents):
    """A limb's bend: of the middles of the rings cut across it (from its end in), the one farthest from the straight line from its
    joint R to its end E (its index), and how far, as a share of the limb's length."""
    L = float(np.linalg.norm(E - R))
    if L < 1e-6 or len(cents) < 3:
        return None, 0.0
    u = (E - R) / L
    best, dev = None, 0.0
    for k, C in enumerate(cents):
        w = C - R
        t = float(w @ u) / L
        d = float(np.linalg.norm(w - (w @ u) * u))
        if 0.15 < t < 0.85 and d > dev:
            best, dev = k, d
    return best, dev / L


def straighten(V, P, R, M, E, outer, d1, d2):
    """A bent limb (joint R, bend M, end E; outer: those of its vertices P beyond the bend) straightened as 3B holds his: all of it
    turned about R so R to M runs along d1, then the part beyond the bend turned about M so M to E runs along d2. Gives the bend
    and the end where they've gone."""
    R1 = rot_between(M - R, d1)
    V[P] = (R1 @ (V[P] - R).T).T + R
    M1 = R1 @ (M - R) + R
    E1 = R1 @ (E - R) + R
    R2 = rot_between(E1 - M1, d2)
    V[outer] = (R2 @ (V[outer] - M1).T).T + M1
    E2 = R2 @ (E1 - M1) + M1
    return M1, E2


def label(nb, n, seeds):
    """Every vertex to the part whose seed is nearest along the surface."""
    lab = -np.ones(n, int)
    dd = np.full(n, np.inf)
    q = []
    for i, k in seeds.items():
        dd[i] = 0.0; lab[i] = k
        q.append((0.0, i))
    heapq.heapify(q)
    while q:
        d, i = heapq.heappop(q)
        if d > dd[i]:
            continue
        for j, w in nb[i].items():
            if d + w < dd[j]:
                dd[j] = d + w; lab[j] = lab[i]
                heapq.heappush(q, (d + w, j))
    lab[lab < 0] = 0
    return lab


def local_ends(nb, V, dist, H, near, far=0.12, apart=0.06, most=48):
    """The vertices farther along the surface than every vertex within two steps of them, far out, apart from each other."""
    n = len(V)
    ends = []
    for i in np.argsort(-dist):
        if dist[i] < far * H:
            break
        ok = True
        for j in nb[i]:
            if dist[j] > dist[i]:
                ok = False; break
            for k in nb[j]:
                if dist[k] > dist[i]:
                    ok = False; break
            if not ok:
                break
        if ok and all(np.linalg.norm(V[i] - V[e]) > apart * H for e in ends):
            ends.append(int(i))
        if len(ends) >= most:
            break
    return ends


def rig(V, T, JT, floor, tail=False, log=print, quadruped=None, hints=None):
    # hints (tools/objchar/build_all.sh, a character's --hints): what a mesh's own shape can't settle, said for it:
    #   legs=none        no legs (a bust, a ghost, a long robe to the floor): all below the waist goes with the body
    #   hip=F            the legs' tops at F of the height (stubby legs under a round body: not where the body narrows)
    #   head=spine       the head goes with the body (a head that is most of the character: it would swing as a whole)
    #   feet=stiff       each foot one piece with its ankle (boots, blocks: a toe of its own only folds the boot)
    #   legs=rigid       each leg one piece on its hip (legs posed wide or bent in the mesh: his knees would fold them the wrong way)
    #   arm+1=none, arm-1=none   that arm stays with the body (held up to the face, a microphone in its hand)
    hints = dict(hints or {})
    V = V.copy()
    T = np.asarray(T)
    n = len(V)
    top = V[:, 1].max()
    H = top - floor
    V[:, 0] -= float(np.median(V[:, 0]))
    zc = float(np.median(V[:, 2]))
    depth = V[:, 2].max() - V[:, 2].min()
    width = V[:, 0].max() - V[:, 0].min()
    if quadruped is None:
        quadruped = depth > 1.15 * H and depth > 1.0 * width
    J = JT.copy().astype(float)
    canon = canon_index(V, H)
    Tc = canon[T]
    nb = graph(V, T, H, canon)
    if quadruped:
        return rig_quadruped(V, T, Tc, J, JT, floor, H, nb, tail, log, hints)
    tol = 0.01 * H

    # ---- the crotch: going up from cuts with the legs apart (a gap in the middle), the first cut with no gap there that stays
    # closed (knees touching close it only for a little)
    def mid_gap(iv):
        return any(iv[k][1] < 0.06 * H and iv[k + 1][0] > -0.06 * H for k in range(len(iv) - 1))
    apart_from = None
    closed = None
    crotch = None
    for f in np.arange(0.03, 0.75, 0.01):
        h = floor + f * H
        iv = slice_x(V, T, h)
        if not iv:
            continue
        if mid_gap(iv):
            if apart_from is None:
                apart_from = h
            closed = None
        elif apart_from is not None:
            if closed is None:
                closed = h
            if h - closed >= 0.08 * H:
                crotch = closed
                break
    if crotch is None and closed is not None:
        crotch = closed
    legs_apart = crotch is not None and crotch - apart_from >= 0.04 * H
    if not legs_apart:
        crotch = floor + 0.25 * H
    widths = []
    for f in np.linspace(0.15, 0.7, 23):
        h = crotch + f * (top - crotch)
        mid = central(slice_x(V, T, h), tol)
        if mid is not None:
            widths.append(max(-mid[0], mid[1]))
    torso_w = float(np.percentile(widths, 25)) if widths else 0.15 * H

    # ---- the neck: down from the top, past the head's widest (where the cut stops growing), the lowest place still about as narrow
    # as the narrowest so far, before the body widens out (the shoulders) below it
    prof = []
    for h in np.arange(top - 0.01 * H, crotch + 0.05 * (top - crotch), -0.01 * H):
        mid = central(slice_x(V, T, h), tol)
        prof.append((h, (mid[1] - mid[0]) if mid is not None else 0.0))
    necks, narrow = neck_candidates(prof, top, H)
    if narrow is not None and narrow > crotch + 0.15 * (top - crotch):
        neck = narrow
    else:
        neck = next((h for h in necks if h < top - 0.12 * H), floor + 0.82 * H)
    if narrow is not None:
        necks = necks + [narrow]

    # ---- the chest, the distances along the surface from it, and the ends
    chest_p = np.array([0.0, crotch + 0.55 * (neck - crotch), zc])
    centre = int(np.argmin(np.linalg.norm(V - chest_p, axis=1) + 2.0 * np.abs(V[:, 0])))
    dist, prev = dijkstra(nb, centre)
    dist[~np.isfinite(dist)] = 0.0
    ends = local_ends(nb, V, dist, H, centre)

    # ---- the limbs. Each one's way back to the chest is cut where the rings across it grow (the limb joining the body); the
    # shoulder no farther in than the torso's side, the hip no higher than the crotch, the neck at the neck
    limbs = []          # (kind, side, end, path beyond the joint, path within, rings beyond, ring centre at the joint)
    taken = set()
    def by_rings(e, inside, min_len, ok=lambda out, c: len(out) >= 2, allow_rule=True):
        p = path_to(prev, e)
        # (the rings: no farther in than a little past the plain rule's cut)
        k_rule = next((k for k, i in enumerate(p) if inside(i)), len(p) - 1)
        P = V[p]
        arc = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        prof = ring_profile(V, Tc, p, H, upto=arc[k_rule] + 0.08 * H)
        for m in find_joint(prof, H, min_len, arc[k_rule] + 0.03 * H, body_r=torso_w):
            k = prof[m][0]
            rings = prof[:m]
            c = prof[m - 1][3] if m > 0 else V[p[k]]
            if rings:
                rmed = float(np.median([q[2] for q in rings if q[1] >= 0.03 * H] or [q[2] for q in rings]))
                # (the joint: the middle of the last ring before it about the limb's own size there (its rings just out from it; not
                # the size of a cane held out past the hand))
                local = [q[2] for q in rings if q[1] >= prof[m][1] - 0.1 * H]
                rloc = float(np.median(local)) if local else rmed
                clean = [q for q in rings if q[2] <= 1.3 * rloc]
                if clean:
                    c = clean[-1][3]
                kept = [q for q in rings if q[2] <= 1.5 * rmed]
            else:
                kept = []
            if ok(p[:k], c):
                by_rings.all = list(prof[:m])
                return p[:k], p[k:], kept, c, 'rings'
        if not allow_rule:
            return [], p, [], V[e], 'none'
        k = k_rule
        before = [q for q in prof if q[0] < k]
        c = before[-1][3] if before else V[p[k]]
        by_rings.all = before
        return p[:k], p[k:], [], c, 'rule'
    for s in (+1, -1):
        if hints.get('arm%+d' % s) == 'none':
            log('arm %+d: with the body (hint)' % s)
            continue
        cands = [e for e in ends if s * V[e][0] > torso_w * 1.1 and floor + 0.02 * H < V[e][1] < top - 0.1 * H and e not in taken]
        # (an end whose joint the rings find first; only failing that, the plain rule's cut)
        found = False
        for rule in (False, True):
            for e in sorted(cands, key=lambda i: -(dist[i] + abs(V[i][0]))):
                arm_ok = lambda out, c: (len(out) >= 2 and np.linalg.norm(c - V[e]) > 0.06 * H
                                         and crotch + 0.1 * (top - crotch) < c[1] < crotch + 0.72 * (top - crotch))
                out, inn, rings, c, how = by_rings(e, lambda i: s * V[i][0] <= torso_w * 0.9, 0.05 * H, arm_ok, allow_rule=rule)
                if how != 'none' and arm_ok(out, c):
                    limbs.append(('arm', s, e, out, inn, rings, c, by_rings.all)); taken.add(e)
                    log('arm %+d: joint by %s' % (s, how))
                    found = True
                    break
            if found:
                break
    # (the neck: of the places it may be, the first not far above the shoulders, nor below them)
    arms = [l[6][1] for l in limbs if l[0] == 'arm']
    if arms and float(np.mean(arms)) >= crotch + 0.5 * (neck - crotch):
        sy = float(np.mean(arms))
        if not (sy - 0.03 * H <= neck <= sy + 0.3 * H):
            ok = [h for h in necks if sy - 0.03 * H <= h <= sy + 0.25 * H]
            neck = max(ok) if ok else min(top - 0.1 * H, sy + 0.06 * H)
    for s in (+1, -1):
        # (its foot: of the vertices on the floor that side, the farthest along the surface from the chest)
        cands = [int(i) for i in np.where((V[:, 1] < floor + 0.05 * H) & (s * V[:, 0] > 0.05 * H))[0] if int(i) not in taken]
        if cands:
            e = max(cands, key=lambda i: dist[i])
            leg_ok = lambda out, c: len(out) >= 2 and c[1] - floor >= 0.12 * H
            out, inn, rings, c, how = by_rings(e, lambda i: V[i][1] >= (crotch + 0.02 * H if legs_apart else floor + 0.45 * H), 0.08 * H, leg_ok)
            if len(out) >= 2 and ((how == 'rings' and c[1] - floor >= 0.12 * H) or (how == 'rule' and legs_apart)):
                limbs.append(('leg', s, e, out, inn, rings, c)); taken.add(e)
                log('leg %+d: joint by %s at %.0f' % (s, how, c[1] - floor))
    # (one leg found alone where the legs aren't apart (a block of trousers, a coat): both sides alike, by the lower body's halves)
    if not legs_apart and sum(1 for l in limbs if l[0] == 'leg') == 1:
        gone = [l for l in limbs if l[0] == 'leg'][0]
        taken.discard(gone[2])
        limbs = [l for l in limbs if l[0] != 'leg']
        log("one leg only, with the legs together: the lower body's halves on the legs instead")
    if hints.get('legs') == 'none' and any(l[0] == 'leg' for l in limbs):
        for l in limbs:
            if l[0] == 'leg':
                taken.discard(l[2])
        limbs = [l for l in limbs if l[0] != 'leg']
        log('no legs (hint)')
    heads = [e for e in ends if V[e][1] > neck and e not in taken]
    if heads:
        e = max(heads, key=lambda i: V[i][1])
        p = path_to(prev, e)
        k = next((k for k, i in enumerate(p) if V[i][1] < neck), len(p) - 1)
        if k >= 1:
            limbs.append(('head', 0, e, p[:k], p[k:], [], V[p[k]])); taken.add(e)
    if tail:
        tails = [e for e in ends if V[e][2] < zc - 0.15 * depth and e not in taken]
        if tails:
            e = min(tails, key=lambda i: V[i][2])
            out, inn, rings, c, how = by_rings(e, lambda i: V[i][2] >= zc - 0.1 * depth, 0.06 * H)
            if len(out) >= 2:
                limbs.append(('tail', 0, e, out, inn, rings, c)); taken.add(e)

    # ---- every vertex to the nearest part along the surface: a limb's rings (and its way out beyond the joint) seed it, its way
    # in and the chest seed the body
    labels = ['body'] + ['%s%+d' % (l[0], l[1]) if l[0] in ('arm', 'leg') else l[0] for l in limbs]
    seeds = {centre: 0}
    # (the torso's core: between the hips and the neck, inside its sides)
    for i in np.where((np.abs(V[:, 0]) <= 0.8 * torso_w) & (V[:, 1] >= crotch + 0.04 * H) & (V[:, 1] <= neck - 0.03 * H))[0]:
        seeds[int(i)] = 0
    for l in limbs:
        for i in l[4]:
            seeds.setdefault(i, 0)
    claim = {}
    for k, l in enumerate(limbs):
        for q in l[5]:
            for i in q[4]:
                if i not in claim or q[2] < claim[i]:
                    claim[i] = q[2]; seeds[i] = k + 1
    for k, l in enumerate(limbs):
        for i in l[3]:
            seeds[i] = k + 1
    # (twins at seams go with each other)
    for i in range(n):
        if canon[i] in seeds and i not in seeds:
            seeds[i] = seeds[canon[i]]
    lab = label(nb, n, seeds)
    keep = set()
    for l in limbs:
        keep.add(l[2]); keep.update(l[3])
    lab = accessories(V, lab, getattr(graph, 'parts', None), keep, tuple([0] + [k + 1 for k, l in enumerate(limbs) if l[0] == 'head']))
    bone = np.full(n, SPINE)

    bent = {}

    # ---- the arms: turned about the shoulder to hang as 3B's, their joints at his proportions along them
    shoulder_ys = []
    for k, l in enumerate(limbs):
        if l[0] != 'arm':
            continue
        s, e, c = l[1], l[2], l[6]
        P = np.where(lab == k + 1)[0]
        if k in bent:
            R, M1, E2, outer = bent[k]
            a = ARM[s]
            J[a[0]] = R; J[a[1]] = M1
            fw = float(np.linalg.norm(JT[a[2]] - JT[a[1]]) / np.linalg.norm(JT[a[3]] - JT[a[1]]))
            J[a[2]] = M1 + (E2 - M1) * fw; J[a[3]] = E2
            J[a[4]] = J[a[2]] + (JT[a[4]] - JT[a[2]]) * float(np.linalg.norm(E2 - M1) / np.linalg.norm(JT[a[3]] - JT[a[1]]))
            u2 = (E2 - M1) / max(1e-6, float(np.linalg.norm(E2 - M1)))
            t2 = (V[P] - M1) @ u2 / max(1e-6, float(np.linalg.norm(E2 - M1)))
            isout = np.array([i in outer for i in P])
            bone[P] = np.where(~isout, a[0], np.where(t2 < fw, a[1], a[2]))
            shoulder_ys.append(R[1])
            log('arm %+d: %d vertices, shoulder at %.0f (straightened)' % (s, len(P), R[1] - floor))
            continue
        sh = c.copy()
        hand = V[e].copy()
        a = ARM[s]
        d0 = JT[a[3]] - JT[a[0]]
        L0 = float(np.linalg.norm(d0))
        allr = l[7] if len(l) > 7 else []
        if len(allr) >= 4:
            # (the hand: walking out from the shoulder, the last ring before a run of much thinner ones: a cane or a staff held out
            # past the hand turns with the hand, the arm is turned by where the hand is)
            rs = np.array([q[2] for q in allr]); cs = np.array([q[3] for q in allr]); ars = np.array([q[1] for q in allr])
            own = float(np.median(rs[ars >= ars[-1] - 0.15 * H]))
            thin = rs < 0.25 * own
            j = len(allr) - 1
            while j >= 3 and not np.all(thin[j - 3:j]):
                j -= 1
            # (a held thing is long and thin all along: most of the way out from there, a third of the height or more)
            if j >= 3 and ars[j] - ars[0] >= 0.3 * H and np.mean(thin[:j]) >= 0.7 and np.linalg.norm(cs[j] - sh) > 0.06 * H:
                hand = cs[j].copy()
                log('arm %+d: held past the hand (%.0f long), turned by the hand' % (s, float(np.linalg.norm(V[e] - hand))))
        turn_from = hand - sh
        L = float(np.linalg.norm(hand - sh))
        R = rot_between(turn_from, d0)
        d0 = JT[a[3]] - JT[a[0]]
        V[P] = (R @ (V[P] - sh).T).T + sh
        kk = L / L0
        for b in a:
            J[b] = sh + (JT[b] - JT[a[0]]) * kk
        u = d0 / L0
        t = ((V[P] - sh) @ u) / L
        te = float(np.dot(J[a[1]] - sh, u)) / L
        tw = float(np.dot(J[a[2]] - sh, u)) / L
        bone[P] = np.where(t < te, a[0], np.where(t < tw, a[1], a[2]))
        shoulder_ys.append(sh[1])
        log('arm %+d: %d vertices, %.0f long, shoulder at %.0f' % (s, len(P), L, sh[1] - floor))

    # ---- the legs: hip, knee, ankle, toe down each
    for k, l in enumerate(limbs):
        if l[0] != 'leg':
            continue
        s = l[1]
        hp, kn, an, to = LEG[s]
        P = np.where(lab == k + 1)[0]
        if len(P) < 3:
            continue
        c = l[6]
        if k in bent:
            R, M1, E2, outer = bent[k]
            ank = M1 + (E2 - M1) * float(np.linalg.norm(JT[an] - JT[kn]) / np.linalg.norm(JT[to] - JT[kn]))
            J[hp], J[kn], J[an], J[to] = R, M1, ank, E2
            isout = np.array([i in outer for i in P])
            bone[P] = np.where(~isout, hp, np.where(V[P, 1] < ank[1] + 0.01 * H, an, kn))
            log('leg %+d: %d vertices, hip at %.0f (straightened)' % (s, len(P), R[1] - floor))
            continue
        hip_y = (min(crotch, c[1]) if legs_apart else c[1]) - 0.02 * H
        if 'hip' in hints:
            # (the hip where it's said to be: the leg is all of the lower body below it on its side (but an arm or a tail hanging
            # there), what its cut took in above it goes with the body)
            hip_y = floor + float(hints['hip']) * H
            mine = (lab == k + 1) | ((lab == 0) & (s * V[:, 0] > 0))
            above = np.where(mine & (V[:, 1] > hip_y + 0.01 * H) & (lab == k + 1))[0]
            lab[above] = 0
            P = np.where(mine & (V[:, 1] <= hip_y + 0.01 * H))[0]
            lab[P] = k + 1
            if len(P) < 3:
                continue
        top_band = P[V[P, 1] > hip_y - 0.12 * (hip_y - floor)]
        low_band = P[V[P, 1] < floor + 0.12 * (hip_y - floor)]
        xc = float(np.median(V[P, 0]))
        hip = np.array([float(c[0]), hip_y, float(c[2])])
        ank_y = floor + 0.06 * (hip_y - floor) + 0.025 * H
        ank = np.array([float(np.mean(V[low_band, 0])) if len(low_band) else xc, ank_y,
                        float(np.percentile(V[low_band, 2], 25)) if len(low_band) else zc])
        knee = (hip + ank) / 2 + np.array([0.0, 0.0, 0.01 * H])
        toe = np.array([ank[0], floor, float(V[low_band, 2].max()) if len(low_band) else ank[2] + 0.05 * H])
        J[hp], J[kn], J[an], J[to] = hip, knee, ank, toe
        y = V[P, 1]
        if hints.get('legs') == 'rigid':
            bone[P] = hp
        elif hints.get('feet') == 'stiff':
            bone[P] = np.where(y < knee[1], kn, hp)
        else:
            bone[P] = np.where(y < ank_y + 0.01 * H, an, np.where(y < knee[1], kn, hp))
        log('leg %+d: %d vertices, hip at %.0f%s' % (s, len(P), hip[1] - floor, ', one piece' if hints.get('legs') == 'rigid' else
                                                     ', its foot with the shin' if hints.get('feet') == 'stiff' else ''))

    # ---- the head, a tail, the body
    shoulder_y = float(np.mean(shoulder_ys)) if shoulder_ys else crotch + 0.7 * (neck - crotch)
    J[ROOT] = [0.0, crotch + 0.03 * H, zc]
    J[SPINE] = [0.0, crotch + 0.3 * (shoulder_y - crotch), zc]
    J[HEAD] = [0.0, neck, zc]
    for b in (13, 14, 15):
        J[b] = J[HEAD] + (JT[b] - JT[HEAD]) * 0.5
    for i, b in enumerate(TAIL):
        J[b] = J[ROOT] + np.array([0.0, 0.0, -0.05 * H * (i + 1)])
    head_bone = SPINE if hints.get('head') == 'spine' else HEAD
    if head_bone != HEAD:
        log('the head with the body (hint)')
    for k, l in enumerate(limbs):
        P = np.where(lab == k + 1)[0]
        if l[0] == 'head':
            bone[P] = head_bone
        elif l[0] == 'tail':
            r0 = l[6]; tip = V[l[2]]
            for i, b in enumerate(TAIL):
                J[b] = r0 + (tip - r0) * (i / 3.0)
            t = ((V[P] - r0) @ (tip - r0)) / max(1e-6, float(np.dot(tip - r0, tip - r0)))
            bone[P] = np.array(TAIL)[np.clip((t * 3).astype(int), 0, 2)]
            log('tail: %d vertices' % len(P))
    body = lab == 0
    bone[body & (V[:, 1] >= neck)] = head_bone
    bone[body & (V[:, 1] < neck) & (V[:, 1] >= J[SPINE][1])] = SPINE
    bone[body & (V[:, 1] < J[SPINE][1])] = ROOT
    # (a side with no leg found apart (a coat): that side of the lower body on its leg's top bone, to go with the other leg as it
    # goes. With no legs at all (a bust, a robe to the floor) the lower body stays with the body: its halves on the hips would tear
    # it apart down the middle at every step.)
    if not any(l[0] == 'leg' for l in limbs):
        # (and the body one piece on the spine, turning about its base: his spine's lean and twist would shear it in two at his
        # spine's joint, the part below on his root)
        J[SPINE] = [0.0, J[ROOT][1], zc]
        bone[body & (bone == ROOT)] = SPINE
        log('no legs: the lower body with the body, all of it on the spine')
    for s in (+1, -1):
        if not any(l[0] == 'leg' for l in limbs) or any(l[0] == 'leg' and l[1] == s for l in limbs):
            continue
        P = np.where(body & (V[:, 1] < crotch) & (s * V[:, 0] > 0.02 * H))[0]
        if len(P):
            bone[P] = LEG[s][0]
            J[LEG[s][0]] = [s * 0.08 * H, crotch, zc]
    log('crotch at %.0f%s, neck at %.0f, torso half width %.0f, of %.0f; limbs %s' % (crotch - floor, '' if legs_apart else ' (legs together)',
        neck - floor, torso_w, H, ' '.join(labels[1:])))
    return V, J, bone


def rig_quadruped(V, T, Tc, J, JT, floor, H, nb, tail, log, hints=None):
    """Four legs, on a two-legged skeleton and its animations: the body (the front legs with it) one rigid piece on 3B's root, each
    back leg one piece on his hip (his steps swing it), the head on his head with his spine's joint put at the neck (his spine's
    lean and twist turn the head about the neck, not the front half of the body about its middle), a tail on his tail.
    (0.11.4: the front half was on his spine and the back legs on his whole legs: every lean bent the body in two, and his knees and
    ankles folded the back legs into spikes.)"""
    n = len(V)
    zmin, zmax = V[:, 2].min(), V[:, 2].max()
    depth = zmax - zmin
    # (its head forward, +z: of its two ends along it, the bulkier one is the head)
    up = V[:, 1] > floor + 0.3 * H
    front = int(np.sum(up & (V[:, 2] > zmax - 0.25 * depth))); back = int(np.sum(up & (V[:, 2] < zmin + 0.25 * depth)))
    if back > 1.15 * front:
        V = V * np.array([-1.0, 1.0, -1.0])
        zmin, zmax = -zmax, -zmin
        log('four legs: turned round, its head was behind (%d vertices at the back end, %d at the front)' % (back, front))
    zc = float(np.median(V[:, 2]))
    # (the feet: the pieces the mesh falls into below the shins (each leg its own), one for each corner)
    lowset = V[:, 1] < floor + 0.15 * H
    low = np.where(lowset)[0]
    cl = {}
    groups = []
    for a in low:
        if a in cl:
            continue
        cl[a] = len(groups); g = [a]; stack = [a]
        while stack:
            i = stack.pop()
            for j in nb[i]:
                if lowset[j] and j not in cl:
                    cl[j] = cl[a]; g.append(j); stack.append(j)
        groups.append(np.array(g))
    zmid = float(np.median(V[low, 2])) if len(low) else zc
    feet = {}
    for g in sorted(groups, key=len, reverse=True):
        if len(g) < 3 or V[g, 1].min() > floor + 0.05 * H:
            continue
        gx, gz = float(np.mean(V[g, 0])), float(np.mean(V[g, 2]))
        key = (1 if gx >= 0 else -1, gz > zmid)
        if key not in feet:
            feet[key] = g
    belly = floor + 0.42 * H
    chest_p = np.array([0.0, belly + 0.2 * H, zc])
    centre = int(np.argmin(np.linalg.norm(V - chest_p, axis=1)))
    dist, prev = dijkstra(nb, centre)
    dist[~np.isfinite(dist)] = 0.0
    ends = local_ends(nb, V, dist, H, centre, far=0.1)
    limbs = []
    def by_rings(e, inside, min_len):
        p = path_to(prev, e)
        k_rule = next((k for k, i in enumerate(p) if inside(i)), len(p) - 1)
        P = V[p]
        arc = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        prof = ring_profile(V, Tc, p, H, upto=arc[k_rule] + 0.08 * H)
        ms = find_joint(prof, H, min_len, arc[k_rule] + 0.03 * H)
        m = ms[0] if ms else None
        if m is not None:
            k = prof[m][0]
            rings = prof[:m]
            c = prof[m - 1][3] if m > 0 else V[p[k]]
        else:
            k = k_rule
            rings = [q for q in prof if q[0] < k]
            c = rings[-1][3] if rings else V[p[k]]
        return p[:k], p[k:], rings, c
    for (s, fwd), g in sorted(feet.items()):
        e = int(g[np.argmax(dist[g])])
        out, inn, rings, c = by_rings(e, lambda i: V[i][1] >= belly, 0.08 * H)
        if len(out) >= 2:
            limbs.append(('front' if fwd else 'back', s, e, out, inn, rings, c))
    heads = [e for e in ends if V[e][2] > zc + 0.25 * depth and V[e][1] > floor + 0.15 * H]
    if heads:
        e = max(heads, key=lambda i: V[i][2])
        out, inn, rings, c = by_rings(e, lambda i: V[i][2] < zmax - 0.3 * depth, 0.1 * H)
        if out:
            limbs.append(('head', 0, e, out, inn, rings, c))
    if tail:
        tails = [e for e in ends if V[e][2] < zc - 0.25 * depth]
        if tails:
            e = min(tails, key=lambda i: V[i][2])
            out, inn, rings, c = by_rings(e, lambda i: V[i][2] > zmin + 0.25 * depth, 0.06 * H)
            if len(out) >= 2:
                limbs.append(('tail', 0, e, out, inn, rings, c))
    seeds = {centre: 0}
    for l in limbs:
        for i in l[4]:
            seeds.setdefault(i, 0)
    for k, l in enumerate(limbs):
        for q in l[5]:
            for i in q[4]:
                seeds[i] = k + 1
        for i in l[3]:
            seeds[i] = k + 1
    lab = label(nb, n, seeds)
    # (each part only past its own joint: the head in front of its neck's cut, a back leg below its hip's, the tail behind its root's.
    # Nearest along the surface alone, the head took the front half of a long body, and turned it with every look)
    head_cut = None
    for k, l in enumerate(limbs):
        kind, c = l[0], l[6]
        mine = lab == k + 1
        if kind == 'head':
            # (no deeper than the front third: a head that runs into a thick neck (a rhino's) found its cut halfway along the body)
            cut = max(float(c[2]), zmax - 0.3 * depth)
            head_cut = cut
            lab[mine & (V[:, 2] < cut - 0.01 * depth)] = 0
        elif kind in ('back', 'front'):
            lab[mine & (V[:, 1] > c[1] + 0.01 * H)] = 0
        elif kind == 'tail':
            lab[mine & (V[:, 2] > c[2] + 0.01 * depth)] = 0
    bone = np.full(n, ROOT)
    J[ROOT] = [0.0, belly + 0.1 * H, zc]
    J[SPINE] = [0.0, belly + 0.15 * H, zc]
    for k, l in enumerate(limbs):
        P = np.where(lab == k + 1)[0]
        kind, s, e, out, c = l[0], l[1], l[2], l[3], l[6]
        if kind == 'back':
            top_ = c.copy(); foot = V[e].copy(); foot[1] = floor + 0.03 * H
            mid = (top_ + foot) / 2
            g = LEG[s]
            J[g[0]], J[g[1]], J[g[2]], J[g[3]] = top_, mid, foot, foot + [0, -0.03 * H, 0.03 * H]
            bone[P] = g[0]
        elif kind == 'front':
            bone[P] = ROOT
        elif kind == 'head':
            J[HEAD] = c.copy()
            if head_cut is not None:
                J[HEAD][2] = max(float(J[HEAD][2]), head_cut)
            J[SPINE] = J[HEAD].copy()
            for b in (13, 14, 15):
                J[b] = J[HEAD]
            bone[P] = HEAD
        elif kind == 'tail':
            r0 = c; tip = V[e]
            for i, b in enumerate(TAIL):
                J[b] = r0 + (tip - r0) * (i / 3.0)
            t = ((V[P] - r0) @ (tip - r0)) / max(1e-6, float(np.dot(tip - r0, tip - r0)))
            bone[P] = np.array(TAIL)[np.clip((t * 3).astype(int), 0, 2)]
    # (his arms (no vertices: what's held in his hands) hang from the neck as from his shoulders)
    for a in (ARM[+1], ARM[-1]):
        for b in a:
            J[b] = J[SPINE] + (JT[b] - JT[SPINE]) * 0.6
    log('four legs: %s; body %.0f long, %.0f high' % (' '.join('%s%+d' % (l[0], l[1]) for l in limbs), depth, H))
    return V, J, bone

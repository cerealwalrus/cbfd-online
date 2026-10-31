# A skinned .dae (Collada) to an OBJ + MTL in its bind pose, for obj_character.py:  python3 dae2obj.py <file.dae> <out folder>  (copy its PNGs beside it)
import sys, os, re, numpy as np, xml.etree.ElementTree as ET
src, out = sys.argv[1], sys.argv[2]
t = ET.parse(src); R = t.getroot()
ns = {'c': R.tag.split('}')[0].strip('{')}
q = lambda e, p: e.find(p, ns); qa = lambda e, p: e.findall(p, ns)
# images, effects, materials
img = {i.get('id'): q(i, 'c:init_from').text for i in qa(R, './/c:library_images/c:image')}
eff_tex = {}
for e in qa(R, './/c:library_effects/c:effect'):
    sampler = {}; surf = {}
    for np_ in qa(e, './/c:newparam'):
        s2 = q(np_, 'c:sampler2D/c:source')
        if s2 is not None: sampler[np_.get('sid')] = s2.text
        sf = q(np_, 'c:surface/c:init_from')
        if sf is not None: surf[np_.get('sid')] = sf.text
    tex = q(e, './/c:diffuse/c:texture')
    if tex is not None:
        k = tex.get('texture'); k = surf.get(sampler.get(k, k), k)
        eff_tex[e.get('id')] = img.get(k, k)
mat = {m.get('id'): eff_tex.get(q(m, 'c:instance_effect').get('url')[1:]) for m in qa(R, './/c:library_materials/c:material')}
def floats(s): return np.array([float(x) for x in s.split()])
V, VT, F, used = [], [], [], {}
skinbsm = {}
for c in qa(R, './/c:library_controllers/c:controller'):
    sk = q(c, 'c:skin'); bsm = q(sk, 'c:bind_shape_matrix')
    skinbsm[sk.get('source')[1:]] = floats(bsm.text).reshape(4, 4) if bsm is not None else np.eye(4)
# instances: controllers and plain geometries with node transform
def node_world(node, parent):
    m = q(node, 'c:matrix'); M = floats(m.text).reshape(4, 4) if m is not None else np.eye(4)
    return parent @ M
inst = []
def walk(node, M):
    W = node_world(node, M)
    for ic in qa(node, 'c:instance_controller'):
        cid = ic.get('url')[1:]; c = R.find(".//c:controller[@id='%s']" % cid, ns)
        g = q(c, 'c:skin').get('source')[1:]
        bm = {im.get('symbol'): im.get('target')[1:] for im in qa(ic, './/c:instance_material')}
        inst.append((g, skinbsm.get(g, np.eye(4)), bm))
    for ig in qa(node, 'c:instance_geometry'):
        bm = {im.get('symbol'): im.get('target')[1:] for im in qa(ig, './/c:instance_material')}
        inst.append((ig.get('url')[1:], W, bm))
    for ch in qa(node, 'c:node'): walk(ch, W)
for vs in qa(R, './/c:library_visual_scenes/c:visual_scene'):
    for n in qa(vs, 'c:node'): walk(n, np.eye(4))
for gid, M, bm in inst:
    g = R.find(".//c:geometry[@id='%s']" % gid, ns); mesh = q(g, 'c:mesh')
    srcs = {s.get('id'): floats(q(s, 'c:float_array').text).reshape(-1, int(q(s, './/c:accessor').get('stride'))) for s in qa(mesh, 'c:source')}
    vin = q(mesh, 'c:vertices'); pos_id = [i.get('source')[1:] for i in qa(vin, 'c:input') if i.get('semantic') == 'POSITION'][0]
    P = srcs[pos_id][:, :3]; P = (np.c_[P, np.ones(len(P))] @ M.T)[:, :3]
    base = len(V); V.extend(P.tolist())
    for tr in qa(mesh, 'c:triangles'):
        ins = qa(tr, 'c:input'); stride = max(int(i.get('offset')) for i in ins) + 1
        vo = [int(i.get('offset')) for i in ins if i.get('semantic') == 'VERTEX'][0]
        tin = [i for i in ins if i.get('semantic') == 'TEXCOORD']
        p = np.array([int(x) for x in q(tr, 'c:p').text.split()]).reshape(-1, stride)
        tb = len(VT)
        if tin:
            T = srcs[tin[0].get('source')[1:]][:, :2]; VT.extend(T.tolist()); to = int(tin[0].get('offset'))
        m = bm.get(tr.get('material'), tr.get('material'))
        for k in range(0, len(p), 3):
            F.append((m, [(base + p[k + j][vo], (tb + p[k + j][to]) if tin else -1) for j in range(3)]))
        used[m] = mat.get(m)
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, 'model.mtl'), 'w') as f:
    for m, tx in used.items():
        f.write('newmtl %s\n' % m)
        if tx: f.write('map_Kd %s\n' % os.path.basename(tx.replace('file://', '')))
with open(os.path.join(out, 'model.obj'), 'w') as f:
    f.write('mtllib model.mtl\n')
    for v in V: f.write('v %f %f %f\n' % tuple(v))
    for vt in VT: f.write('vt %f %f\n' % tuple(vt))
    cur = None
    for m, c in F:
        if m != cur: f.write('usemtl %s\n' % m); cur = m
        f.write('f ' + ' '.join('%d/%s' % (a + 1, (b + 1) if b >= 0 else '') for a, b in c) + '\n')
V = np.array(V); print(len(V), 'verts', len(F), 'tris', len(used), 'materials', V.min(0), V.max(0), q(R, './/c:up_axis').text if q(R, './/c:up_axis') is not None else '')

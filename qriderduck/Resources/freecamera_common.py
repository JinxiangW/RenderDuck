"""Matrix and shader-read helpers for data-only replay recipes."""
import math
import re
import riderduck as rd

def transpose(m):
    return [m[c * 4 + r] for r in range(4) for c in range(4)]


def multiply(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
            for r in range(4) for c in range(4)]


def inverse(m):
    rows = [list(m[r * 4:r * 4 + 4]) + [float(r == c) for c in range(4)]
            for r in range(4)]
    for c in range(4):
        pivot = max(range(c, 4), key=lambda r: abs(rows[r][c]))
        if abs(rows[pivot][c]) < 1e-10:
            raise ValueError('Singular matrix')
        rows[c], rows[pivot] = rows[pivot], rows[c]
        scale = rows[c][c]
        rows[c] = [v / scale for v in rows[c]]
        for r in range(4):
            if r != c:
                scale = rows[r][c]
                rows[r] = [a - scale * b for a, b in zip(rows[r], rows[c])]
    return [v for row in rows for v in row[4:]]


def close(a, b, absolute=0.002, relative=0.00002):
    return (len(a) == len(b) and all(math.isfinite(x) and math.isfinite(y) and
            abs(x - y) <= absolute + relative * max(abs(x), abs(y))
            for x, y in zip(a, b)))


IDENTITY = [float(r == c) for r in range(4) for c in range(4)]
STAGES = ((rd.ShaderStage.Vertex, 'VS'), (rd.ShaderStage.Hull, 'HS'),
          (rd.ShaderStage.Domain, 'DS'), (rd.ShaderStage.Geometry, 'GS'),
          (rd.ShaderStage.Pixel, 'PS'), (rd.ShaderStage.Compute, 'CS'))

def registers(code, slot):
    # Declarations describe capacity, not which camera fields are actually read.
    instructions = '\n'.join(line for line in code.splitlines()
                             if re.match(r'^\s*\d+:', line) and
                             not re.search(r'\bdcl_', line))
    operands = re.findall(r'\bcb{}\[([^\]]+)\]'.format(slot), instructions)
    return {int(x) for x in operands if x.isdigit()}, any(not x.isdigit() for x in operands)


def flatten(actions):
    for action in actions:
        yield action
        yield from flatten(action.children)

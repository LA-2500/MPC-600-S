# Makes os/helper_sprite.h: the help screens' helper (traced from img references/animecharacter.bmp)
# and her animation frames. Run: python3 tools/helper_sprite.py os/helper_sprite.h
base = """\
......###.....###.....
.....#...##.##...#....
.....#....###....#....
......###..#..###.....
.....############.....
.....#############....
.....##############...
....########.######...
....#######...#####...
...####........#####..
...###..#....#..####..
...###.##...##..####..
..####..........#####.
...####..##....#####..
.....###......####....
....#...#######...#...
...#.......#.......#..
..#...#....#....#..#..
.#...##....#....##..#.
#...#.#.........#.#..#
#.....#.........#....#
#..#..#.........#..#.#
.#.#..#.........#.#..#
..#.#.#.........##..#.
...#.###.......##..#..
....#...#.....#...#...
.....#..#.....#..#....
......##.......##.....
......#.........#.....
.....#...........#....
.....#...........#....
.....#...........#....
.....#############....
......#.#.....#.#.....
......#.#.....#.#.....
......#.#.....#.#.....
......#.#.....#.#.....
......#.#.....#.#.....
......#.#.....#.#.....
.....####.....####....
...##...#.....#...##..
...######.....######..""".split('\n')
PAD = 12
W = 22 + PAD
def grid(rows):
    return [list('.' * PAD + r) for r in rows]
def line(g, x0, y0, x1, y1, ch='#'):
    n = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(n + 1):
        x = round(x0 + (x1 - x0) * i / n); y = round(y0 + (y1 - y0) * i / n)
        if 0 <= y < len(g) and 0 <= x < W: g[y][x] = ch
def tap(g): # her right foot (the viewer's right) lifted a row
    for y in range(33, 42):
        for x in range(PAD + 13, W): g[y][x] = '.'
    for y in range(33, 38):
        g[y][PAD + 14] = '#'; g[y][PAD + 16] = '#'
    shoe = ["####.", "#...##", "######"]
    for i, r in enumerate(shoe):
        for j, ch in enumerate(r):
            if ch == '#': g[38 + i][PAD + 14 + j] = '#'
def point(g, stick_y):
    # Her left arm (the viewer's left) out at shoulder height, the pointer up at the text.
    # The arm and the hand that was on her hip gone, all of it, down to the waist.
    for y in range(15, 27):
        for x in range(PAD, PAD + 8): g[y][x] = '.'
    line(g, PAD + 4, 15, PAD - 3, 15)   # top of the arm
    line(g, PAD + 5, 17, PAD - 3, 17)   # underside
    for y in range(17, 24): g[y][PAD + 6] = '#'  # her side, in to the waist
    g[24][PAD + 7] = '#'
    for y in range(14, 19):              # the hand, round the pointer
        for x in range(PAD - 6, PAD - 3): g[y][x] = '#'
    g[16][PAD - 5] = '.'
    line(g, PAD - 6, 14, 0, stick_y)    # the pointer
frames = []
f = grid(base); frames.append(f)
f = grid(base); tap(f); frames.append(f)
f = grid(base); point(f, 1); frames.append(f)
f = grid(base); point(f, 4); frames.append(f)
for i, f in enumerate(frames):
    print('frame', i)
    for r in f: print(''.join(r))
with open(__import__('sys').argv[1], 'w') as out:
    out.write('// Generated from the img references sprite (22 x 42) and its animation frames.\n')
    out.write('#define HELPER_W %d\n#define HELPER_H %d\n#define HELPER_FRAMES %d\n' % (W, len(base), len(frames)))
    out.write('static const char *const HELPER[HELPER_FRAMES][HELPER_H] = {\n')
    for f in frames:
        out.write('    {\n')
        for r in f: out.write('        "%s",\n' % ''.join(r))
        out.write('    },\n')
    out.write('};\n')

#include "lcd.h"
#include "lcd_font.h"
#include "lcd_font_xl.h"
#include <stdio.h>
#include <string.h>

uint32_t lcd_bg = 0x408cfa, lcd_fg = 0xe6f0fa;
int lcd_layout = LAYOUT_DEFAULT;

const char *lcd_layout_name(int i) {
    static const char *const NAMES[LAYOUTS] = {"DEFAULT", "2000XL"};
    return i >= 0 && i < LAYOUTS ? NAMES[i] : "";
}

static const unsigned char NOTE_GLYPH[7] = {0x02, 0x02, 0x02, 0x02, 0x0e, 0x1e, 0x0c};

// A character's seven rows of dots, in the layout's letters.
static const unsigned char *glyph_rows(char c) {
    if (c == LCD_NOTE) return NOTE_GLYPH;
    int index = (unsigned char)c - 32;
    if (index < 0 || index >= 95) return 0;
    return lcd_layout == LAYOUT_2000XL ? LCD_FONT_XL[index] : LCD_FONT[index];
}

static const struct {
    const char *name;
    uint32_t bg, fg;
} SCHEMES[SCREEN_SCHEMES] = {
    {"MPC3000 BLUE", 0x408cfa, 0xe6f0fa},
    {"MPC60 GREEN", 0x86cf74, 0x0e220c},
    {"MPC1000 GREEN", 0xa9c652, 0x1c2a12}, // the yellow-green backlight, near-black dots
    {"MPC4000", 0xc9d5c4, 0x26312c},       // grey-green white
    {"MPC2000", 0xd9b67e, 0x3a2918},       // amber
    {"BLACK/RED", 0x000000, 0xff2a1a},
    {"RED/BLACK", 0xd42a1e, 0x000000},
    {"FAIRLIGHT", 0x000000, 0x36ff6a},     // green on black
};

const char *lcd_scheme_name(int i) {
    return i >= 0 && i < SCREEN_SCHEMES ? SCHEMES[i].name : "";
}

void lcd_set_scheme(int i) {
    if (i < 0 || i >= SCREEN_SCHEMES) i = 0;
    lcd_bg = SCHEMES[i].bg, lcd_fg = SCHEMES[i].fg;
}

void lcd_dot(struct ui *u, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x + 1 >= UI_W || y + 1 >= UI_H) return;
    u->px[y * UI_W + x] = u->px[y * UI_W + x + 1] = color;
    u->px[(y + 1) * UI_W + x] = u->px[(y + 1) * UI_W + x + 1] = color;
}

void lcd_fill(struct ui *u, int x, int y, int w, int h, uint32_t color) {
    for (int r = y < 0 ? 0 : y; r < y + h && r < UI_H; r++)
        for (int c = x < 0 ? 0 : x; c < x + w && c < UI_W; c++) u->px[r * UI_W + c] = color;
}

void lcd_char(struct ui *u, int col, int row, char c, uint32_t color) {
    lcd_glyph(u, LCD_X + col * CELL_W, row * CELL_H, c, color);
}

void lcd_glyph(struct ui *u, int x0, int y0, char c, uint32_t color) {
    const unsigned char *g = glyph_rows(c);
    if (!g) return;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 5; x++)
            if (g[y] & (0x10 >> x)) lcd_dot(u, x0 + x * 2, y0 + y * 2, color);
}

void lcd_small_text(struct ui *u, int x0, int y0, const char *s, uint32_t color) {
    for (; *s; s++, x0 += 6) {
        const unsigned char *g = glyph_rows(*s);
        if (!g) continue;
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if ((g[y] & (0x10 >> x)) && x0 + x < UI_W && y0 + y < UI_H && x0 + x >= 0 && y0 + y >= 0)
                    u->px[(y0 + y) * UI_W + x0 + x] = color;
    }
}

void lcd_text(struct ui *u, int col, int row, const char *s) {
    size_t n = strlen(s);
    // The MPC2000XL layout: a function key's <Name> is a tab, as on its F keys.
    if (lcd_layout == LAYOUT_2000XL && row == KEY_ROW && n > 2 && s[0] == '<' && s[n - 1] == '>') {
        char label[LCD_COLS + 1];
        snprintf(label, sizeof label, "%.*s", (int)n - 2, s + 1);
        lcd_tab(u, col, (int)n, label, 0);
        return;
    }
    for (; *s && col < LCD_COLS; s++, col++) lcd_char(u, col, row, *s, LCD_FG);
}

void lcd_box(struct ui *u, int x, int y, int w, int h) {
    lcd_fill(u, x, y, w, 2, LCD_FG);
    lcd_fill(u, x, y + h - 2, w, 2, LCD_FG);
    lcd_fill(u, x, y, 2, h, LCD_FG);
    lcd_fill(u, x + w - 2, y, 2, h, LCD_FG);
}

void lcd_dotted(struct ui *u, int x, int y, int w) {
    for (int i = 0; i + 2 <= w; i += 4) lcd_fill(u, x + i, y, 2, 2, LCD_FG);
}

void lcd_tab(struct ui *u, int col, int n, const char *label, int filled) {
    if (col + n > LCD_COLS) n = LCD_COLS - col;
    int x = LCD_X + col * CELL_W - 2, w = n * CELL_W + 2, len = (int)strlen(label);
    if (len > n) len = n;
    int tx = LCD_X + col * CELL_W + (n - len) * CELL_W / 2;
    // Into the blank row under the keys, so the frame clears the letters.
    if (filled) lcd_fill(u, x, 0, w, CELL_H + 4, LCD_FG);
    else lcd_box(u, x, 0, w, CELL_H + 4);
    for (int i = 0; i < len; i++) lcd_glyph(u, tx + i * CELL_W, 3, label[i], filled ? LCD_BG : LCD_FG);
}

void lcd_inverse(struct ui *u, int col, int row, const char *s) {
    int n = (int)strlen(s);
    if (lcd_layout == LAYOUT_2000XL && row == KEY_ROW && n > 2 && s[0] == '<' && s[n - 1] == '>') { // the page shown: a filled tab
        char label[LCD_COLS + 1];
        snprintf(label, sizeof label, "%.*s", n - 2, s + 1);
        lcd_tab(u, col, n, label, 1);
        return;
    }
    if (col + n > LCD_COLS) n = LCD_COLS - col;
    lcd_fill(u, LCD_X + col * CELL_W - 1, row * CELL_H - 1, n * CELL_W, CELL_H, LCD_FG);
    for (int i = 0; i < n; i++) lcd_char(u, col + i, row, s[i], LCD_BG);
}

void lcd_center(struct ui *u, int row, const char *s) {
    int n = (int)strlen(s);
    lcd_text(u, n < LCD_COLS ? (LCD_COLS - n) / 2 : 0, row, s);
}

void lcd_text_y(struct ui *u, int col, int y, const char *s) {
    for (; *s && col < LCD_COLS; s++, col++) lcd_glyph(u, LCD_X + col * CELL_W, y, *s, LCD_FG);
}

void lcd_rule(struct ui *u, int row, const char *title) {
    lcd_rule_y(u, row * CELL_H, title);
}

void lcd_rule_y(struct ui *u, int y, const char *title) {
    if (lcd_layout == LAYOUT_2000XL) { // a dotted line, the title on it
        lcd_dotted(u, 0, y + 7, UI_W);
        int n = title ? (int)strlen(title) : 0;
        if (!n) return;
        if (n + 2 > LCD_COLS) {
            int w = n * 6 - 1, x = LCD_X + (LCD_COLS * CELL_W - w) / 2;
            lcd_fill(u, x - 4, y, w + 8, CELL_H, LCD_BG);
            lcd_small_text(u, x, y + (CELL_H - 7) / 2, title, LCD_FG);
            return;
        }
        int col = (LCD_COLS - n - 2) / 2;
        lcd_fill(u, LCD_X + col * CELL_W, y, (n + 2) * CELL_W, CELL_H, LCD_BG);
        lcd_text_y(u, col + 1, y, title);
        return;
    }
    for (int c = 0; c < LCD_COLS; c++) lcd_glyph(u, LCD_X + c * CELL_W, y, '=', LCD_FG);
    int n = title ? (int)strlen(title) : 0;
    if (!n) return;
    if (n + 2 > LCD_COLS) { // too long for the big letters: the small ones, so all of it shows
        int w = n * 6 - 1, x = LCD_X + (LCD_COLS * CELL_W - w) / 2;
        lcd_fill(u, x - 4, y, w + 8, CELL_H, LCD_BG);
        lcd_small_text(u, x, y + (CELL_H - 7) / 2, title, LCD_FG);
        return;
    }
    int col = (LCD_COLS - n - 2) / 2;
    lcd_fill(u, LCD_X + col * CELL_W, y, (n + 2) * CELL_W, CELL_H, LCD_BG);
    lcd_text_y(u, col + 1, y, title);
}


void lcd_art(struct ui *u, int x, int y, const char *const *rows, int n) {
    for (int r = 0; r < n; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#') lcd_dot(u, x + c * 2, y + r * 2, LCD_FG);
}

void lcd_line(struct ui *u, int x0, int y0, int x1, int y1) { // in pixels, drawn in 2x2 dots
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1, n = (dx > dy ? dx : dy) / 2;
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) lcd_dot(u, (x0 + (x1 - x0) * i / n) & ~1, (y0 + (y1 - y0) * i / n) & ~1, LCD_FG);
}

const char *const LCD_ART_CARD[16] = { // a floppy, as the XL draws its disk, with SD on its label
    "##############..", "#.##########.#..", "#.#......#.#.##.", "#.#......#.#..##", "#.##########...#",
    "#..............#", "#.############.#", "#.#..........#.#", "#.#.###.##...#.#", "#.#.#...#.#..#.#",
    "#.#.###.#.#..#.#", "#.#...#.#.#..#.#", "#.#.###.##...#.#", "#.#..........#.#", "#.############.#",
    "################"};
const char *const LCD_ART_MPC[14] = { // the MPC: its screen, a knob, the pads
    "##############", "#............#", "#.########...#", "#.#......#.#.#", "#.########...#", "#............#",
    "#.##...#.#.#.#", "#.##.........#", "#......#.#.#.#", "#.##.........#", "#.##...#.#.#.#", "#............#",
    "#............#", "##############"};
const char *const LCD_ART_ARROW[11] = {
    "......#.....", "......##....", "......#.#...", "#######..#..", "#.........#.", "#..........#",
    "#.........#.", "#######..#..", "......#.#...", "......##....", "......#....."};
const char *const LCD_ART_METRO[21] = { // the XL's metronome: its weight out to the left, on its base
    ".......###......", "......#...#.....", "#.....#...#.....", ".##..#.....#....", "#..##......#....",
    "#..##......#....", ".####......#....", "...##.......#...", "...###......#...", "...#.#......#...",
    "...#..#.....#...", "..#...#......#..", "..#....#.....#..", "..#.....#....#..", "..#.....#....#..",
    "..############..", ".###############", ".#.............#", ".#.............#", ".#.............#",
    ".###############"};

#include "lcd.h"
#include "lcd_font.h"
#include <string.h>

uint32_t lcd_bg = 0x408cfa, lcd_fg = 0xe6f0fa;

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
    int index = (unsigned char)c - 32;
    if (index < 0 || index >= 95) return;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 5; x++)
            if (LCD_FONT[index][y] & (0x10 >> x)) lcd_dot(u, x0 + x * 2, y0 + y * 2, color);
}

void lcd_small_text(struct ui *u, int x0, int y0, const char *s, uint32_t color) {
    for (; *s; s++, x0 += 6) {
        int index = (unsigned char)*s - 32;
        if (index < 0 || index >= 95) continue;
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if ((LCD_FONT[index][y] & (0x10 >> x)) && x0 + x < UI_W && y0 + y < UI_H && x0 + x >= 0 && y0 + y >= 0)
                    u->px[(y0 + y) * UI_W + x0 + x] = color;
    }
}

void lcd_text(struct ui *u, int col, int row, const char *s) {
    for (; *s && col < LCD_COLS; s++, col++) lcd_char(u, col, row, *s, LCD_FG);
}

void lcd_inverse(struct ui *u, int col, int row, const char *s) {
    int n = (int)strlen(s);
    if (col + n > LCD_COLS) n = LCD_COLS - col;
    lcd_fill(u, LCD_X + col * CELL_W - 1, row * CELL_H - 1, n * CELL_W, CELL_H, LCD_FG);
    for (int i = 0; i < n; i++) lcd_char(u, col + i, row, s[i], LCD_BG);
}

void lcd_center(struct ui *u, int row, const char *s) {
    int n = (int)strlen(s);
    lcd_text(u, n < LCD_COLS ? (LCD_COLS - n) / 2 : 0, row, s);
}

void lcd_rule(struct ui *u, int row, const char *title) {
    for (int c = 0; c < LCD_COLS; c++) lcd_char(u, c, row, '=', LCD_FG);
    int n = title ? (int)strlen(title) : 0;
    if (!n) return;
    if (n + 2 > LCD_COLS) { // too long for the big letters: the small ones, so all of it shows
        int w = n * 6 - 1, x = LCD_X + (LCD_COLS * CELL_W - w) / 2;
        lcd_fill(u, x - 4, row * CELL_H, w + 8, CELL_H, LCD_BG);
        lcd_small_text(u, x, row * CELL_H + (CELL_H - 7) / 2, title, LCD_FG);
        return;
    }
    int col = (LCD_COLS - n - 2) / 2;
    lcd_fill(u, LCD_X + col * CELL_W, row * CELL_H, (n + 2) * CELL_W, CELL_H, LCD_BG);
    lcd_text(u, col + 1, row, title);
}

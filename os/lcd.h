// The MPC3000-style character display: a 26 x 15 grid of 5x7 characters, one color on blue.
#pragma once
#include "panel.h"
#include <stdint.h>

// The screen's two colours, set on Additional Settings (Screen:); the MPC3000's blue to start.
extern uint32_t lcd_bg, lcd_fg;
#define LCD_BG lcd_bg
#define LCD_FG lcd_fg
// The colour schemes: background and characters.
enum { SCREEN_MPC3000, SCREEN_MPC60, SCREEN_MPC1000, SCREEN_MPC2000, SCREEN_MPC2000_AGED, SCREEN_BLACK_RED,
       SCREEN_RED_BLACK, SCREEN_FAIRLIGHT, SCREEN_SCHEMES };
const char *lcd_scheme_name(int i);
void lcd_set_scheme(int i);
#define LCD_X 4
#define CELL_W 12
#define CELL_H 16
#define LCD_COLS 26
#define KEY_ROW 0 // directly under the physical B1, B2, B3
#define TITLE_ROW 2 // row 1 stays blank under the function keys
#define BOTTOM_ROW 14

void lcd_dot(struct ui *u, int x, int y, uint32_t color); // one 2x2 LCD pixel
void lcd_fill(struct ui *u, int x, int y, int w, int h, uint32_t color);
void lcd_char(struct ui *u, int col, int row, char c, uint32_t color);
void lcd_glyph(struct ui *u, int x, int y, char c, uint32_t color); // a character at any pixel position
void lcd_text(struct ui *u, int col, int row, const char *s);
// The Layout setting (Additional Settings): DEFAULT, or the MPC2000XL's: its letters on every
// page, function keys as tabs, dotted rules, and its main screen and Track Mute page.
enum { LAYOUT_DEFAULT, LAYOUT_2000XL, LAYOUTS };
extern int lcd_layout;
const char *lcd_layout_name(int i);
#define LCD_NOTE '\x01' // a crotchet, for the tempo
void lcd_box(struct ui *u, int x, int y, int w, int h);  // a frame of 2-pixel lines
// A picture in LCD dots ('#' lit), its top left at pixel x, y; and a line of dots between two points.
void lcd_art(struct ui *u, int x, int y, const char *const *rows, int n);
void lcd_line(struct ui *u, int x0, int y0, int x1, int y1);
// The MPC2000XL's pictures: the card (SD where the XL says HD), the MPC, the arrow between them,
// and the metronome. Width and height in dots.
extern const char *const LCD_ART_CARD[16], *const LCD_ART_MPC[14], *const LCD_ART_ARROW[11], *const LCD_ART_METRO[21];
void lcd_dotted(struct ui *u, int x, int y, int w);      // a line of dots
// A function key's tab over cols col..col+n-1: filled (dark letters on a bar) or an outline.
void lcd_tab(struct ui *u, int col, int n, const char *label, int filled);
// A light bar with dark characters, the display's way of marking the current field.
void lcd_inverse(struct ui *u, int col, int row, const char *s);
void lcd_center(struct ui *u, int row, const char *s);
// The same characters at half size (one screen pixel a dot), at any pixel position: 6 x 8 each.
void lcd_small_text(struct ui *u, int x, int y, const char *s, uint32_t color);
// ===== Title =====, or a plain rule when title is NULL.
void lcd_rule(struct ui *u, int row, const char *title);
// The same at any pixel height: pages that sit lower on the 2000XL layout, under its taller tabs.
void lcd_rule_y(struct ui *u, int y, const char *title);
void lcd_text_y(struct ui *u, int col, int y, const char *s);
#define XL_DROP (lcd_layout == LAYOUT_2000XL ? 8 : 0) // pixels such a page moves down

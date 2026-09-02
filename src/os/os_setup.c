/*
 * AuroraOS first-time setup wizard + USER.dat persistence.
 *
 * Built and laid out from the mockup PNGs. The flow has five steps, shown on the
 * top screen's progress bar: Language -> Network -> User Details -> Personalise
 * -> Welcome. Everything is driven with the D-pad and A/B/START (the 3DS has no
 * touch driver yet), and Wi-Fi is not implemented, so the Network step only
 * offers a Skip button as requested.
 *
 * The user's selections are written to SD:\Aurora\USER.dat, a fixed 64-byte
 * record beginning with the magic "ADAT". On later boots os_main() loads this
 * file and applies it, so setup only runs once.
 */
#include "aurora.h"
#include "aurora_logo.h"
#include "ff.h"
#include "font.h"
#include "lang.h"
#include "user.h"

/* From os_main.c (declared in aurora.h): delay(), get_keys_down(). */

/* ============================ localization ============================== */
/* One row per string id (see lang.h); columns are English / Espanol / Francais.
 * Font is ASCII-only, so translations drop accents. */
int g_lang = 0;

static const char *const T[STR_COUNT][LANG_COUNT] = {
    /* STR_LANGUAGE     */ {"Language", "Idioma", "Langue"},
    /* STR_NETWORK      */ {"Network", "Red", "Reseau"},
    /* STR_DETAILS      */ {"Details", "Datos", "Profil"},
    /* STR_PERSONAL     */ {"Personal", "Color", "Couleur"},
    /* STR_WELCOME      */ {"Welcome", "Listo", "Pret"},
    /* STR_GET_STARTED  */ {"Get started", "Comenzar", "Commencer"},
    /* STR_NET_L1       */
    {"Set up a wireless network connection to use",
     "Configura una conexion de red inalambrica",
     "Configurez une connexion reseau sans fil"},
    /* STR_NET_L2       */
    {"online features such as the Aurora Store.",
     "para usar funciones como la Aurora Store.",
     "pour les fonctions en ligne (Aurora Store)."},
    /* STR_WIFI_UNAVAIL */
    {"Wi-Fi is not available yet.", "El Wi-Fi aun no esta disponible.",
     "Le Wi-Fi n'est pas encore disponible."},
    /* STR_SKIP         */ {"Skip", "Omitir", "Passer"},
    /* STR_NET_HINT     */
    {"A / START: Skip    B: Back", "A / START: Omitir   B: Atras",
     "A / START: Passer   B: Retour"},
    /* STR_USER_L1      */
    {"Enter your details so Aurora knows what to",
     "Introduce tus datos para que Aurora sepa",
     "Entrez vos infos pour qu'Aurora sache"},
    /* STR_USER_L2      */
    {"call you and when your birthday is.",
     "como llamarte y tu fecha de nacimiento.",
     "comment vous appeler et votre naissance."},
    /* STR_USER_NAME    */ {"User name", "Nombre", "Nom"},
    /* STR_DAY          */ {"Day", "Dia", "Jour"},
    /* STR_MONTH        */ {"Month", "Mes", "Mois"},
    /* STR_YEAR         */ {"Year", "Ano", "Annee"},
    /* STR_BACK         */ {"Back", "Atras", "Retour"},
    /* STR_NEXT         */ {"Next", "Siguiente", "Suivant"},
    /* STR_USER_HINT    */
    {"<>: move  ^v: change  A: select", "<>: mover  ^v: cambiar  A: elegir",
     "<>: bouger ^v: changer A: choisir"},
    /* STR_KB_ENTER_NAME*/
    {"Enter your user name", "Escribe tu nombre", "Entrez votre nom"},
    /* STR_PERS_L1      */
    {"Pick an accent colour for the Aurora interface.",
     "Elige un color de acento para Aurora.",
     "Choisissez une couleur d'accent pour Aurora."},
    /* STR_PERS_HINT    */
    {"D-Pad: choose   A: Next   B: Back", "D-Pad: elegir  A: Sig.  B: Atras",
     "D-Pad: choisir A: Suiv. B: Retour"},
    /* STR_PRESS_A_START*/
    {"Press (A) to start using Aurora!", "Pulsa (A) para empezar con Aurora!",
     "Appuyez sur (A) pour demarrer Aurora!"},
    /* STR_B_BACK       */ {"B: Back", "B: Atras", "B: Retour"},
    /* STR_SETTINGS     */ {"Settings", "Ajustes", "Reglages"},
    /* STR_WIFI         */ {"Wi-Fi", "Wi-Fi", "Wi-Fi"},
    /* STR_ACCENT_COLOR */
    {"Accent Color", "Color de acento", "Couleur d'accent"},
    /* STR_BRIGHTNESS   */ {"Brightness", "Brillo", "Luminosite"},
    /* STR_ABOUT        */ {"About", "Acerca", "A propos"},
    /* STR_OFF          */ {"Off", "No", "Non"},
    /* STR_PICK_ACCENT  */
    {"Pick an accent colour", "Elige un color", "Choisir une couleur"},
    /* STR_A_APPLY_B_BACK*/
    {"A: Apply   B: Back", "A: Aplicar  B: Atras", "A: Appliquer B: Retour"},
    /* STR_POWER_OFF    */ {"Power Off", "Apagar", "Eteindre"},
    /* STR_SYSTEM       */ {"System", "Sistema", "Systeme"},
    /* STR_EMPTY_SLOT   */ {"Empty Slot", "Vacio", "Vide"},
    /* STR_LOADING      */ {"Loading...", "Cargando...", "Chargement..."},
    /* STR_SOUND_TEST   */ {"Sound Test", "Sonido", "Son"},
    /* STR_MUSIC        */ {"Music", "Musica", "Musique"},
    /* STR_NO_TRACKS    */
    {"No .aaf files found", "No hay archivos .aaf", "Aucun fichier .aaf"},
    /* STR_PLAYING      */ {"Playing", "Reproduciendo", "Lecture"},
    /* STR_STOPPED      */ {"Stopped", "Detenido", "Arrete"},
    /* STR_DEBUG_CRASH  */
    {"Force Debug Crash", "Forzar Fallo", "Forcer un Crash"},
    /* STR_TOUCH_TEST   */ {"Touch Test", "Tactil", "Tactile"},
};

const char *L(StringId id) {
  int l = g_lang;
  if (l < 0 || l >= LANG_COUNT)
    l = 0;
  if ((unsigned)id >= (unsigned)STR_COUNT)
    return "";
  return T[id][l];
}

/* ============================ shared accent palette ====================== */
/* Index 0 is Aurora teal, the default. Ordered to roughly match the swatch
 * grid in mockup/setup6.png. Also used by the Settings accent picker. */
const Color aurora_accent_presets[AURORA_ACCENT_COUNT] = {
    {0x64, 0xE8, 0xC8}, /* Aurora     */
    {0xFF, 0x3B, 0x30}, /* Red        */
    {0xFF, 0x9F, 0x0A}, /* Orange     */
    {0xFF, 0xD6, 0x0A}, /* Yellow     */
    {0x34, 0xC8, 0x3A}, /* Green      */
    {0x1E, 0x8A, 0x3B}, /* Forest     */
    {0x32, 0xD6, 0xE2}, /* Cyan       */
    {0x3B, 0x82, 0xF6}, /* Blue       */
    {0x28, 0x2F, 0xE6}, /* Indigo     */
    {0xA0, 0x5C, 0xE2}, /* Purple     */
    {0xFF, 0x2D, 0xB8}, /* Magenta    */
    {0xFF, 0x2D, 0x55}, /* Rose       */
    {0xB0, 0xB8, 0xE8}, /* Periwinkle */
    {0x9A, 0x9A, 0x9A}, /* Gray       */
};
const char *aurora_accent_names[AURORA_ACCENT_COUNT] = {
    "Aurora", "Red",    "Orange",  "Yellow", "Green",      "Forest", "Cyan",
    "Blue",   "Indigo", "Purple",  "Magenta", "Rose",      "Periwinkle", "Gray",
};

/* ============================ small utilities =========================== */

static u32 slen(const char *s) {
  u32 n = 0;
  while (*s++)
    n++;
  return n;
}

/* Unsigned int -> decimal string (no sign). */
static void uint_str(u32 v, char *out) {
  char tmp[12];
  int i = 0;
  if (v == 0)
    tmp[i++] = '0';
  while (v) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  int p = 0;
  while (i)
    out[p++] = tmp[--i];
  out[p] = '\0';
}

/* Transparent text: draws only set glyph pixels (no background box), so it sits
 * cleanly over the gradient/glow on the welcome screen. */
static void text_tr(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg) {
  int cx = x;
  while (*s) {
    char c = *s++;
    if (c < 0x20 || c > 0x7E) {
      cx += FONT_WIDTH;
      continue;
    }
    const uint8_t *g = font_data[c - 0x20];
    for (int row = 0; row < FONT_HEIGHT; row++) {
      uint8_t bits = g[row];
      for (int col = 0; col < FONT_WIDTH; col++)
        if (bits & (0x80 >> col))
          draw_pixel(fb, cx + col, y + row, sh, fg);
    }
    cx += FONT_WIDTH;
  }
}

static void text_center_tr(volatile u8 *fb, int y, int screen_w, int sh,
                           const char *s, Color fg) {
  int x = (screen_w - (int)slen(s) * FONT_WIDTH) / 2;
  text_tr(fb, x, y, sh, s, fg);
}

/* Filled disc (used to build the progress icons). */
static void disc(volatile u8 *fb, int cx, int cy, int r, int sh, Color c) {
  draw_filled_round_rect(fb, cx - r, cy - r, 2 * r, 2 * r, r, sh, c);
}

/* Thick line as a run of small blocks (progress check mark). */
static void thick_line(volatile u8 *fb, int x0, int y0, int x1, int y1, int t,
                       int sh, Color c) {
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps < 1)
    steps = 1;
  for (int i = 0; i <= steps; i++) {
    int x = x0 + dx * i / steps;
    int y = y0 + dy * i / steps;
    draw_filled_rect(fb, x, y, t, t, sh, c);
  }
}

/* ============================ top-screen chrome ========================= */

#define SH_TOP TOP_SCREEN_HEIGHT

static void status_bar(void) {
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 22, SH_TOP,
                   COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 10, 7, SH_TOP, "12:08", COLOR_WHITE, COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 60, 7, SH_TOP, "Fri 28 Aug", COLOR_HM_TEXT2,
              COLOR_HM_BAR);
}

/* One progress icon centred at (cx,cy) in the given colour. */
static void step_icon(int step, int cx, int cy, Color col) {
  volatile u8 *fb = VRAM_TOP_LA;
  switch (step) {
    case 0: /* Language: globe (ring + crosshair) */
      disc(fb, cx, cy, 9, SH_TOP, col);
      disc(fb, cx, cy, 6, SH_TOP, COLOR_HM_BG);
      draw_filled_rect(fb, cx - 1, cy - 9, 2, 18, SH_TOP, col);
      draw_filled_rect(fb, cx - 9, cy - 1, 18, 2, SH_TOP, col);
      break;
    case 1: /* Network: three rising signal bars */
      for (int b = 0; b < 3; b++) {
        int h = 4 + b * 4;
        draw_filled_rect(fb, cx - 9 + b * 6, cy + 6 - h, 4, h, SH_TOP, col);
      }
      break;
    case 2: /* User Details: person */
      disc(fb, cx, cy - 5, 4, SH_TOP, col);
      draw_filled_round_rect(fb, cx - 7, cy + 1, 14, 9, 6, SH_TOP, col);
      break;
    case 3: /* Personalise: gear (ring + 4 teeth) */
      disc(fb, cx, cy, 8, SH_TOP, col);
      disc(fb, cx, cy, 4, SH_TOP, COLOR_HM_BG);
      draw_filled_rect(fb, cx - 2, cy - 11, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx - 2, cy + 7, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx - 11, cy - 2, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx + 7, cy - 2, 4, 4, SH_TOP, col);
      break;
    case 4: /* Welcome: check mark */
      thick_line(fb, cx - 7, cy, cx - 2, cy + 5, 3, SH_TOP, col);
      thick_line(fb, cx - 2, cy + 5, cx + 8, cy - 7, 3, SH_TOP, col);
      break;
    default:
      break;
  }
}

/* Progress bar: completed steps white, the active step in `accent`, future
 * steps dimmed. Labels STR_LANGUAGE..STR_WELCOME are consecutive (0..4). */
static void step_bar(int active, Color accent) {
  static const int cxs[5] = {40, 120, 200, 280, 360};
  for (int i = 0; i < 5; i++) {
    Color col =
        (i == active) ? accent : (i < active ? COLOR_WHITE : COLOR_HM_TEXT2);
    const char *lab = L((StringId)(STR_LANGUAGE + i));
    step_icon(i, cxs[i], 166, col);
    int lx = cxs[i] - (int)slen(lab) * FONT_WIDTH / 2;
    text_tr(VRAM_TOP_LA, lx, 194, SH_TOP, lab, col);
  }
}

/* Standard step top screen: status bar, up to two lines of body copy, and the
 * progress bar with `step` active in the current accent colour. */
static void setup_top(int step, const char *l1, const char *l2, Color accent) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  status_bar();
  if (l1)
    text_center_tr(VRAM_TOP_LA, 78, TOP_SCREEN_WIDTH, SH_TOP, l1, COLOR_WHITE);
  if (l2)
    text_center_tr(VRAM_TOP_LA, 94, TOP_SCREEN_WIDTH, SH_TOP, l2, COLOR_WHITE);
  step_bar(step, accent);
}

/* ============================ bottom-screen widgets ===================== */

#define SH_BOT BOT_SCREEN_HEIGHT

static void button(int x, int y, int w, int h, const char *label, int sel,
                   Color accent) {
  draw_filled_round_rect(VRAM_BOT_A, x - 2, y - 2, w + 4, h + 4, 10, SH_BOT,
                         sel ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, x, y, w, h, 8, SH_BOT, COLOR_HM_SLOT);
  int tl = (int)slen(label) * FONT_WIDTH;
  draw_string(VRAM_BOT_A, x + (w - tl) / 2, y + (h - FONT_HEIGHT) / 2, SH_BOT,
              label, COLOR_WHITE, COLOR_HM_SLOT);
}

static void bottom_title(const char *s) {
  draw_string(VRAM_BOT_A, 12, 12, SH_BOT, s, COLOR_HM_TEXT2, COLOR_HM_BG);
}

/* ============================ step 1: Language ========================== */
/* Branded welcome screen (mockup/setup1.png): green glow, AURORA wordmark and
 * version on top; language list + Get started on the bottom. */

static void glow_top(void) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  int cx = TOP_SCREEN_WIDTH / 2, cy = 95;
  for (int y = 22; y < 185; y++) {
    for (int x = 0; x < TOP_SCREEN_WIDTH; x++) {
      int dx = x - cx, dy = (y - cy) * 2;
      int d2 = dx * dx + dy * dy;
      int inten = 120 - d2 / 70;
      if (inten <= 0)
        continue;
      if (inten > 120)
        inten = 120;
      Color c = {(u8)(0x16 + inten * 0x0E / 120),
                 (u8)(0x16 + inten * 0x5E / 120),
                 (u8)(0x16 + inten * 0x46 / 120)};
      draw_pixel(VRAM_TOP_LA, x, y, SH_TOP, c);
    }
  }
  status_bar();
  text_center_tr(VRAM_TOP_LA, 56, TOP_SCREEN_WIDTH, SH_TOP, "Welcome to",
                 COLOR_WHITE);
  int lx = (TOP_SCREEN_WIDTH - AURORA_LOGO_WIDTH) / 2;
  draw_aurora_logo(VRAM_TOP_LA, lx, 78, SH_TOP, COLOR_WHITE);
  text_tr(VRAM_TOP_LA, 12, 214, SH_TOP, "v0.0.7", COLOR_HM_TEXT2);
}

static const char *lang_names[LANG_COUNT] = {"English", "Espanol", "Francais"};

static void lang_bottom(int sel, Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  int rx = 12, rw = BOT_SCREEN_WIDTH - 24, rh = 40, y0 = 34, step = 48;
  for (int i = 0; i < LANG_COUNT; i++) {
    int y = y0 + i * step;
    int on = (i == sel);
    draw_filled_round_rect(VRAM_BOT_A, rx, y, rw, rh, 8, SH_BOT,
                           on ? accent : COLOR_HM_SLOT);
    draw_string(VRAM_BOT_A, rx + 16, y + (rh - FONT_HEIGHT) / 2, SH_BOT,
                lang_names[i], COLOR_WHITE, on ? accent : COLOR_HM_SLOT);
    if (on) /* check mark on the selected language */
      thick_line(VRAM_BOT_A, rx + rw - 30, y + rh / 2, rx + rw - 24,
                 y + rh / 2 + 6, 2, SH_BOT, COLOR_WHITE),
          thick_line(VRAM_BOT_A, rx + rw - 24, y + rh / 2 + 6, rx + rw - 14,
                     y + rh / 2 - 6, 2, SH_BOT, COLOR_WHITE);
  }
  button((BOT_SCREEN_WIDTH - 200) / 2, BOT_SCREEN_HEIGHT - 38, 200, 30,
         L(STR_GET_STARTED), 1, accent);
  screen_present_bottom();
}

static void step_language(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  g_lang = cfg->language;
  glow_top();
  screen_present_top();
  int sel = cfg->language;
  lang_bottom(sel, accent);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < LANG_COUNT - 1)
      sel++;
    if (sel != prev) {
      cfg->language = (u8)sel;
      g_lang = sel; /* re-render the button etc. in the chosen language */
      lang_bottom(sel, accent);
    }
    if (k & (BUTTON_A | BUTTON_START)) {
      cfg->language = (u8)sel;
      return; /* Get started -> next step (no Back on the first screen) */
    }
    delay(60000);
  }
}

/* ============================ step 2: Network =========================== */
/* Wi-Fi is not implemented yet, so per the brief this screen offers only a Skip
 * button (B still steps back). */

typedef enum { NAV_NEXT, NAV_BACK } Nav;

static Nav step_network(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  setup_top(1, L(STR_NET_L1), L(STR_NET_L2), accent);
  screen_present_top();

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  text_center_tr(VRAM_BOT_A, 70, BOT_SCREEN_WIDTH, SH_BOT, L(STR_WIFI_UNAVAIL),
                 COLOR_HM_TEXT2);
  button((BOT_SCREEN_WIDTH - 180) / 2, 110, 180, 40, L(STR_SKIP), 1, accent);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 22, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_NET_HINT), COLOR_HM_TEXT2);
  screen_present_bottom();

  while (1) {
    u32 k = get_keys_down();
    if (k & (BUTTON_A | BUTTON_START))
      return NAV_NEXT;
    if (k & BUTTON_B)
      return NAV_BACK;
    delay(60000);
  }
}

/* ============================ on-screen keyboard ======================== */
/* D-pad navigable QWERTY for the user-name field (no touch driver yet). */

static const char *kb_rows[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};
static const char *kb_special[4] = {"Caps", "Space", "Del", "OK"};

static int kb_row_len(int row) {
  if (row < 4)
    return (int)slen(kb_rows[row]);
  return 4; /* action row */
}

static char kb_apply_caps(char c, int caps) {
  if (caps && c >= 'a' && c <= 'z')
    return (char)(c - 'a' + 'A');
  return c;
}

static void kb_draw(const char *name, int row, int col, int caps,
                    Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);

  /* Text field showing the name being typed (or a placeholder). */
  draw_filled_round_rect(VRAM_BOT_A, 12, 10, BOT_SCREEN_WIDTH - 24, 30, 8,
                         SH_BOT, COLOR_HM_SLOT);
  if (name[0])
    draw_string(VRAM_BOT_A, 20, 18, SH_BOT, name, COLOR_WHITE, COLOR_HM_SLOT);
  else
    draw_string(VRAM_BOT_A, 20, 18, SH_BOT, L(STR_KB_ENTER_NAME),
                COLOR_HM_TEXT2, COLOR_HM_SLOT);

  int kw = 27, kh = 26, gap = 3, y0 = 52, ystep = kh + gap;
  for (int r = 0; r < 4; r++) {
    int n = kb_row_len(r);
    int roww = n * kw + (n - 1) * gap;
    int x0 = (BOT_SCREEN_WIDTH - roww) / 2;
    for (int c = 0; c < n; c++) {
      int x = x0 + c * (kw + gap), y = y0 + r * ystep;
      int on = (r == row && c == col);
      draw_filled_round_rect(VRAM_BOT_A, x, y, kw, kh, 5, SH_BOT,
                             on ? accent : COLOR_HM_SLOT);
      char ch[2] = {kb_apply_caps(kb_rows[r][c], caps), 0};
      draw_string(VRAM_BOT_A, x + (kw - FONT_WIDTH) / 2,
                  y + (kh - FONT_HEIGHT) / 2, SH_BOT, ch, COLOR_WHITE,
                  on ? accent : COLOR_HM_SLOT);
    }
  }
  /* Action row: Caps / Space / Del / OK. */
  int ay = y0 + 4 * ystep;
  int aw[4] = {56, 96, 56, 56}, ax = 20;
  for (int c = 0; c < 4; c++) {
    int on = (row == 4 && col == c);
    int w = aw[c];
    Color face = COLOR_HM_SLOT;
    if (c == 0 && caps)
      face = accent; /* Caps lit when active */
    draw_filled_round_rect(VRAM_BOT_A, ax - 2, ay - 2, w + 4, kh + 4, 6, SH_BOT,
                           on ? accent : COLOR_HM_BG);
    draw_filled_round_rect(VRAM_BOT_A, ax, ay, w, kh, 5, SH_BOT, face);
    int tl = (int)slen(kb_special[c]) * FONT_WIDTH;
    draw_string(VRAM_BOT_A, ax + (w - tl) / 2, ay + (kh - FONT_HEIGHT) / 2,
                SH_BOT, kb_special[c], COLOR_WHITE, face);
    ax += w + 8;
  }
  screen_present_bottom();
}

/* Edit `name` (buffer of USER_NAME_MAX incl. NUL) on the keyboard. */
static void keyboard_edit(char *name, Color accent) {
  int row = 1, col = 0, caps = 0;
  int len = (int)slen(name);
  kb_draw(name, row, col, caps, accent);
  while (1) {
    u32 k = get_keys_down();
    int changed = 0;

    if (k & BUTTON_DUP) {
      row = (row > 0) ? row - 1 : 4;
      changed = 1;
    }
    if (k & BUTTON_DDOWN) {
      row = (row < 4) ? row + 1 : 0;
      changed = 1;
    }
    if (changed) {
      int n = kb_row_len(row);
      if (col >= n)
        col = n - 1;
    }
    if (k & BUTTON_DLEFT) {
      col = (col > 0) ? col - 1 : kb_row_len(row) - 1;
      changed = 1;
    }
    if (k & BUTTON_DRIGHT) {
      col = (col < kb_row_len(row) - 1) ? col + 1 : 0;
      changed = 1;
    }

    int commit = 0; /* pressed a key with A */
    char typed = 0;
    if (k & BUTTON_A) {
      if (row < 4) {
        typed = kb_apply_caps(kb_rows[row][col], caps);
      } else {
        switch (col) {
          case 0:
            caps = !caps;
            break;
          case 1:
            typed = ' ';
            break;
          case 2: /* Del */
            if (len > 0)
              name[--len] = '\0';
            break;
          case 3: /* OK */
            return;
        }
      }
      commit = 1;
    }
    if (k & BUTTON_B) { /* quick backspace */
      if (len > 0)
        name[--len] = '\0';
      commit = 1;
    }
    if (k & BUTTON_START) /* quick done */
      return;
    if (k & BUTTON_L) { /* quick caps toggle */
      caps = !caps;
      commit = 1;
    }

    if (typed && len < USER_NAME_MAX - 1) {
      name[len++] = typed;
      name[len] = '\0';
    }

    if (changed || commit)
      kb_draw(name, row, col, caps, accent);
    delay(60000);
  }
}

/* ============================ step 3: User Details ====================== */
/* Focus order: 0 Name, 1 Day, 2 Month, 3 Year, 4 Back, 5 Next.
 * Left/Right move focus; Up/Down adjust the focused date field. */

static void clamp_date(UserConfig *cfg) {
  if (cfg->birth_day < 1)
    cfg->birth_day = 1;
  if (cfg->birth_day > 31)
    cfg->birth_day = 31;
  if (cfg->birth_month < 1)
    cfg->birth_month = 1;
  if (cfg->birth_month > 12)
    cfg->birth_month = 12;
  if (cfg->birth_year < 1900)
    cfg->birth_year = 1900;
  if (cfg->birth_year > 2025)
    cfg->birth_year = 2025;
}

static void date_box(int x, int y, const char *label, u32 val, int digits,
                     int sel, Color accent) {
  draw_string(VRAM_BOT_A, x, y - 14, SH_BOT, label, COLOR_HM_TEXT2, COLOR_HM_BG);
  int w = (digits == 4) ? 68 : 52, h = 40;
  draw_filled_round_rect(VRAM_BOT_A, x - 2, y - 2, w + 4, h + 4, 8, SH_BOT,
                         sel ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, x, y, w, h, 6, SH_BOT, COLOR_HM_SLOT);
  char s[8];
  uint_str(val, s);
  int n = (int)slen(s);
  int tl = n * FONT_WIDTH;
  draw_string(VRAM_BOT_A, x + (w - tl) / 2, y + (h - FONT_HEIGHT) / 2, SH_BOT, s,
              COLOR_WHITE, COLOR_HM_SLOT);
}

static void user_bottom(const UserConfig *cfg, int focus, Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");

  /* Name field */
  draw_filled_round_rect(VRAM_BOT_A, 12, 30, BOT_SCREEN_WIDTH - 24, 34,
                         8, SH_BOT, (focus == 0) ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, 15, 33, BOT_SCREEN_WIDTH - 30, 28, 6,
                         SH_BOT, COLOR_HM_SLOT);
  if (cfg->name[0])
    draw_string(VRAM_BOT_A, 24, 40, SH_BOT, cfg->name, COLOR_WHITE,
                COLOR_HM_SLOT);
  else
    draw_string(VRAM_BOT_A, 24, 40, SH_BOT, L(STR_USER_NAME), COLOR_HM_TEXT2,
                COLOR_HM_SLOT);

  /* Date row */
  date_box(40, 96, L(STR_DAY), cfg->birth_day, 2, focus == 1, accent);
  date_box(120, 96, L(STR_MONTH), cfg->birth_month, 2, focus == 2, accent);
  date_box(200, 96, L(STR_YEAR), cfg->birth_year, 4, focus == 3, accent);

  /* Buttons */
  button(40, 168, 100, 32, L(STR_BACK), focus == 4, accent);
  button(180, 168, 100, 32, L(STR_NEXT), focus == 5, accent);

  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 16, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_USER_HINT), COLOR_HM_TEXT2);
  screen_present_bottom();
}

static Nav step_user(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  setup_top(2, L(STR_USER_L1), L(STR_USER_L2), accent);
  screen_present_top();

  clamp_date(cfg);
  int focus = 0;
  user_bottom(cfg, focus, accent);
  while (1) {
    u32 k = get_keys_down();
    int redraw = 0;

    if (k & BUTTON_DLEFT) {
      focus = (focus > 0) ? focus - 1 : 5;
      redraw = 1;
    }
    if (k & BUTTON_DRIGHT) {
      focus = (focus < 5) ? focus + 1 : 0;
      redraw = 1;
    }
    if (k & (BUTTON_DUP | BUTTON_DDOWN)) {
      int d = (k & BUTTON_DUP) ? 1 : -1;
      if (focus == 1)
        cfg->birth_day = (u8)(cfg->birth_day + d);
      else if (focus == 2)
        cfg->birth_month = (u8)(cfg->birth_month + d);
      else if (focus == 3)
        cfg->birth_year = (u16)(cfg->birth_year + d);
      clamp_date(cfg);
      redraw = 1;
    }
    if (k & BUTTON_A) {
      if (focus == 0) {
        keyboard_edit(cfg->name, accent);
        setup_top(2, L(STR_USER_L1), L(STR_USER_L2), accent);
        screen_present_top();
        redraw = 1;
      } else if (focus == 4) {
        return NAV_BACK;
      } else if (focus == 5) {
        return NAV_NEXT;
      }
    }
    if (k & BUTTON_B)
      return NAV_BACK;
    if (k & BUTTON_START)
      return NAV_NEXT;

    if (redraw)
      user_bottom(cfg, focus, accent);
    delay(60000);
  }
}

/* ============================ step 4: Personalise ====================== */

#define PSW 36
#define PGAP 10
#define PCOLS 6
#define PGW (PCOLS * PSW + (PCOLS - 1) * PGAP)
#define PX ((BOT_SCREEN_WIDTH - PGW) / 2)
#define PY 44
#define PROWS ((AURORA_ACCENT_COUNT + PCOLS - 1) / PCOLS)

static void accent_bottom(int sel) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  for (int i = 0; i < AURORA_ACCENT_COUNT; i++) {
    int col = i % PCOLS, row = i / PCOLS;
    int x = PX + col * (PSW + PGAP), y = PY + row * (PSW + PGAP);
    if (i == sel) /* selection ring */
      draw_filled_round_rect(VRAM_BOT_A, x - 4, y - 4, PSW + 8, PSW + 8, PSW / 2,
                             SH_BOT, COLOR_WHITE);
    disc(VRAM_BOT_A, x + PSW / 2, y + PSW / 2, PSW / 2, SH_BOT,
         (i == sel) ? COLOR_HM_BG : aurora_accent_presets[i]);
    disc(VRAM_BOT_A, x + PSW / 2, y + PSW / 2,
         (i == sel) ? PSW / 2 - 4 : PSW / 2, SH_BOT, aurora_accent_presets[i]);
  }
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 16, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_PERS_HINT), COLOR_HM_TEXT2);
  screen_present_bottom();
}

static Nav step_personalise(UserConfig *cfg) {
  int sel = cfg->accent;
  setup_top(3, L(STR_PERS_L1), 0, aurora_accent_presets[sel]);
  screen_present_top();
  accent_bottom(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    int col = sel % PCOLS, row = sel / PCOLS;
    if ((k & BUTTON_DLEFT) && col > 0)
      sel--;
    if ((k & BUTTON_DRIGHT) && col < PCOLS - 1 && sel + 1 < AURORA_ACCENT_COUNT)
      sel++;
    if ((k & BUTTON_DUP) && row > 0)
      sel -= PCOLS;
    if ((k & BUTTON_DDOWN) && row < PROWS - 1 &&
        sel + PCOLS < AURORA_ACCENT_COUNT)
      sel += PCOLS;
    if (sel != prev) {
      cfg->accent = (u8)sel;
      /* Live preview: recolour the progress bar's active step. */
      setup_top(3, L(STR_PERS_L1), 0, aurora_accent_presets[sel]);
      screen_present_top();
      accent_bottom(sel);
    }
    if (k & (BUTTON_A | BUTTON_START)) {
      cfg->accent = (u8)sel;
      return NAV_NEXT;
    }
    if (k & BUTTON_B)
      return NAV_BACK;
    delay(60000);
  }
}

/* ============================ step 5: Welcome ========================== */

static void console_icon(int cx, int cy, Color color) {
  volatile u8 *fb = VRAM_TOP_LA;
  draw_filled_round_rect(fb, cx - 34, cy - 44, 68, 40, 8, SH_TOP, color);
  draw_filled_round_rect(fb, cx - 26, cy - 38, 52, 26, 4, SH_TOP, COLOR_HM_BG);
  draw_filled_round_rect(fb, cx - 34, cy, 68, 44, 8, SH_TOP, color);
  draw_filled_round_rect(fb, cx - 20, cy + 6, 34, 26, 4, SH_TOP, COLOR_HM_BG);
  draw_filled_rect(fb, cx + 18, cy + 14, 3, 9, SH_TOP, COLOR_HM_BG);
  draw_filled_rect(fb, cx + 15, cy + 17, 9, 3, SH_TOP, COLOR_HM_BG);
}

static Nav step_welcome(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  status_bar();
  console_icon(TOP_SCREEN_WIDTH / 2, 70, COLOR_WHITE);
  step_bar(4, accent);
  screen_present_top();

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  text_center_tr(VRAM_BOT_A, 100, BOT_SCREEN_WIDTH, SH_BOT, L(STR_PRESS_A_START),
                 COLOR_WHITE);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_B_BACK), COLOR_HM_TEXT2);
  screen_present_bottom();

  while (1) {
    u32 k = get_keys_down();
    if (k & (BUTTON_A | BUTTON_START))
      return NAV_NEXT;
    if (k & BUTTON_B)
      return NAV_BACK;
    delay(60000);
  }
}

/* ============================ wizard driver ============================ */

void setup_run(UserConfig *cfg) {
  g_lang = cfg->language;
  int step = 0;
  while (step < 5) {
    Nav n = NAV_NEXT;
    switch (step) {
      case 0:
        step_language(cfg); /* first screen: Get started only */
        n = NAV_NEXT;
        break;
      case 1:
        n = step_network(cfg);
        break;
      case 2:
        n = step_user(cfg);
        break;
      case 3:
        n = step_personalise(cfg);
        break;
      case 4:
        n = step_welcome(cfg);
        break;
    }
    if (n == NAV_BACK) {
      if (step > 0)
        step--;
    } else {
      step++;
    }
  }
  cfg->setup_done = 1;
  cfg->valid = 1;
}

/* ============================ USER.dat load / save ===================== */

static FATFS s_fs;
static FIL s_fil;
static u8 s_buf[USER_DAT_SIZE];

void user_config_defaults(UserConfig *cfg) {
  cfg->valid = 0;
  cfg->setup_done = 0;
  cfg->language = LANG_ENGLISH;
  cfg->accent = 0;
  cfg->birth_day = 1;
  cfg->birth_month = 1;
  cfg->birth_year = 2000;
  for (int i = 0; i < USER_NAME_MAX; i++)
    cfg->name[i] = 0;
}

int user_config_load(UserConfig *cfg) {
  user_config_defaults(cfg);

  if (f_mount(&s_fs, "", 1) != FR_OK)
    return 0;
  FRESULT fr = f_open(&s_fil, USER_DAT_PATH, FA_READ);
  if (fr != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  UINT br = 0;
  fr = f_read(&s_fil, s_buf, USER_DAT_SIZE, &br);
  f_close(&s_fil);
  f_mount(NULL, "", 0);
  if (fr != FR_OK || br < 12)
    return 0;

  /* Only trust the file if it carries the "ADAT" magic. */
  if (s_buf[0] != 'A' || s_buf[1] != 'D' || s_buf[2] != 'A' || s_buf[3] != 'T')
    return 0;

  cfg->setup_done = s_buf[5];
  cfg->language = s_buf[6];
  cfg->accent = s_buf[7];
  cfg->birth_day = s_buf[8];
  cfg->birth_month = s_buf[9];
  cfg->birth_year = (u16)(s_buf[10] | (s_buf[11] << 8));
  int i = 0;
  for (; i < USER_NAME_MAX - 1 && (12 + i) < (int)br; i++)
    cfg->name[i] = (char)s_buf[12 + i];
  cfg->name[i] = '\0';

  if (cfg->language >= LANG_COUNT)
    cfg->language = LANG_ENGLISH;
  if (cfg->accent >= AURORA_ACCENT_COUNT)
    cfg->accent = 0;
  clamp_date(cfg);
  cfg->valid = 1;
  return 1;
}

int user_config_save(const UserConfig *cfg) {
  if (f_mount(&s_fs, "", 1) != FR_OK)
    return 0;
  f_mkdir(USER_DAT_DIR); /* ignore FR_EXIST / already-present */

  for (int i = 0; i < USER_DAT_SIZE; i++)
    s_buf[i] = 0;
  s_buf[0] = 'A';
  s_buf[1] = 'D';
  s_buf[2] = 'A';
  s_buf[3] = 'T';
  s_buf[4] = USER_DAT_VERSION;
  s_buf[5] = cfg->setup_done;
  s_buf[6] = cfg->language;
  s_buf[7] = cfg->accent;
  s_buf[8] = cfg->birth_day;
  s_buf[9] = cfg->birth_month;
  s_buf[10] = (u8)(cfg->birth_year & 0xFF);
  s_buf[11] = (u8)(cfg->birth_year >> 8);
  for (int i = 0; i < USER_NAME_MAX; i++)
    s_buf[12 + i] = (u8)cfg->name[i];

  FRESULT fr = f_open(&s_fil, USER_DAT_PATH, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  UINT bw = 0;
  fr = f_write(&s_fil, s_buf, USER_DAT_SIZE, &bw);
  f_close(&s_fil);
  f_mount(NULL, "", 0);
  return (fr == FR_OK && bw == USER_DAT_SIZE);
}

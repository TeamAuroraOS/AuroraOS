/* Aurora accounts: the device flow of the account server's API.md on
 * 3ds.aurora3ds.xyz, the token file, and Settings > Aurora Account. See
 * docs/account.md. */
#include "account.h"
#include "anim.h"
#include "assets.h"
#include "ff.h"
#include "fileview.h"
#include "http.h"
#include "json.h"
#include "lang.h"
#include "model.h"
#include "qr.h"
#include "statusbar.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include "wifi.h"

#define AC_CLIENT "aurora-3ds"
#define AC_TOKEN_LEN 43 /* device_code and access_token */
#define AC_CODE_LEN  9  /* user_code, XXXX-XXXX */
#define AC_NAME_MAX  24 /* the console's name on the account */

static char ac_token[AC_TOKEN_LEN + 1];
static char ac_user[24];
static FATFS ac_fs;

static int ac_len(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static char *ac_cpy(char *d, const char *s) {
  while ((*d = *s)) {
    d++;
    s++;
  }
  return d;
}

static char *ac_num(char *d, u32 v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n)
    *d++ = t[--n];
  *d = 0;
  return d;
}

static void ac_wipe(char *s, int n) {
  volatile char *v = s;
  for (int i = 0; i < n; i++)
    v[i] = 0;
}

/* `line` starting with `key`: the rest of it into `out`. */
static int ac_kv(const char *line, const char *key, char *out, int size) {
  int k = 0, n = 0;
  for (; key[k]; k++)
    if (line[k] != key[k])
      return 0;
  while (line[k + n] && n < size - 1) {
    out[n] = line[k + n];
    n++;
  }
  out[n] = 0;
  return 1;
}

/* Tokens and device codes are base64url. */
static int ac_token_ok(const char *t) {
  int n = 0;
  for (; t[n]; n++) {
    char c = t[n];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return 0;
  }
  return n == AC_TOKEN_LEN;
}

void account_load(void) {
  static FIL f;
  static char buf[160];
  UINT br = 0;
  ac_token[0] = 0;
  ac_user[0] = 0;
  if (f_mount(&ac_fs, "", 1) == FR_OK &&
      f_open(&f, ACCOUNT_FILE, FA_READ) == FR_OK) {
    if (f_read(&f, buf, sizeof(buf) - 1, &br) != FR_OK)
      br = 0;
    f_close(&f);
  }
  f_mount(NULL, "", 0);
  buf[br] = 0;
  for (char *l = buf; *l;) {
    char *e = l;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    int more = *e != 0;
    *e = 0;
    if (!ac_kv(l, "token=", ac_token, sizeof(ac_token)))
      ac_kv(l, "username=", ac_user, sizeof(ac_user));
    l = more ? e + 1 : e;
  }
  ac_wipe(buf, sizeof(buf));
  if (!ac_token_ok(ac_token)) {
    ac_token[0] = 0;
    ac_user[0] = 0;
  }
}

int account_linked(void) { return ac_token[0] != 0; }

const char *account_token(void) { return ac_token[0] ? ac_token : 0; }

const char *account_name(void) { return ac_token[0] ? ac_user : ""; }

static int ac_store(const char *token, const char *user) {
  static FIL f;
  static char buf[128];
  char *p = ac_cpy(buf, "token=");
  p = ac_cpy(p, token);
  p = ac_cpy(p, "\r\nusername=");
  p = ac_cpy(p, user);
  p = ac_cpy(p, "\r\n");
  UINT n = (UINT)(p - buf), bw = 0;
  int ok = 0;
  if (f_mount(&ac_fs, "", 1) == FR_OK) {
    f_mkdir("Aurora");
    if (f_open(&f, ACCOUNT_FILE, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
      ok = f_write(&f, buf, n, &bw) == FR_OK && bw == n;
      ok = f_close(&f) == FR_OK && ok;
    }
  }
  f_mount(NULL, "", 0);
  ac_wipe(buf, sizeof(buf));
  if (ok) {
    ac_kv(token, "", ac_token, sizeof(ac_token));
    ac_kv(user, "", ac_user, sizeof(ac_user));
  }
  return ok;
}

int account_forget(void) {
  int ok = 0;
  if (f_mount(&ac_fs, "", 1) == FR_OK) {
    FRESULT fr = f_unlink(ACCOUNT_FILE);
    ok = fr == FR_OK || fr == FR_NO_FILE;
  }
  f_mount(NULL, "", 0);
  if (ok) {
    ac_wipe(ac_token, sizeof(ac_token));
    ac_wipe(ac_user, sizeof(ac_user));
  }
  return ok;
}

/* The name the console links under, in the 1 to 24 printable ASCII
 * characters the server takes: "<owner>'s New 3DS", accents dropped. */
static void ac_console_name(const char *owner, char *out) {
  static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                              "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
  const char *model = aurora_is_new3ds() ? "New 3DS" : "3DS";
  int room = AC_NAME_MAX - 3 - ac_len(model), n = 0;
  const u8 *s = (const u8 *)owner;
  while (*s && n < room) {
    u32 c = *s++;
    if (c >= 0xC0u && c < 0xE0u && (*s & 0xC0u) == 0x80u)
      c = ((c & 0x1Fu) << 6) | (*s++ & 0x3Fu);
    else if (c >= 0x80u)
      while ((*s & 0xC0u) == 0x80u)
        s++;
    if (c >= 0xC0u && c <= 0xFFu)
      c = (u8)latin[c - 0xC0u];
    if (c < 0x20u || c > 0x7Eu || (c == ' ' && n == 0))
      continue;
    out[n++] = (char)c;
  }
  while (n && out[n - 1] == ' ')
    n--;
  char *p = n ? out + n : ac_cpy(out, "AuroraOS");
  p = ac_cpy(p, n ? "'s " : " ");
  ac_cpy(p, model);
}

typedef struct {
  u32 net;    /* WIFI_NETS_* */
  u32 stage;  /* WIFI_HTTP_* */
  int status; /* HTTP status, 0 when no reply was read */
  int ok;     /* the body is a JSON object */
  Json j;
} AcReply;

static char ac_resp[4096 + 1];
static JsonTok ac_tok[32];

/* Drawn while a network command runs: CPU only (see wifi_direct_on). */
static void (*ac_tick)(u32 ms);

static int ac_hex(int v) { return v < 10 ? '0' + v : 'A' + v - 10; }

/* key=value at p, the value percent-encoded but for A-Z a-z 0-9 - . _ ~.
 * A form starts one byte into a buffer whose first byte is 0; any later
 * pair goes after an '&'. Returns the end. */
static char *ac_form(char *p, const char *key, const char *val) {
  if (p[-1] != 0)
    *p++ = '&';
  p = ac_cpy(p, key);
  *p++ = '=';
  for (const u8 *s = (const u8 *)val; *s; s++) {
    u32 c = *s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
        c == '~') {
      *p++ = (char)c;
    } else {
      *p++ = '%';
      *p++ = (char)ac_hex((int)(c >> 4));
      *p++ = (char)ac_hex((int)(c & 15u));
    }
  }
  *p = 0;
  return p;
}

/* One request to ACCOUNT_HOST on the joined network, its reply read as
 * JSON. Returns the HTTP status, 0 when no reply came (r->net says why). */
static int ac_call(const char *method, const char *path, const char *form,
                   int auth, AcReply *r) {
  HttpReply h;
  r->status = http_call(method, path, auth ? ac_token : 0,
                        "Accept: application/json\r\n", form, ac_resp,
                        sizeof(ac_resp) - 1, &h, ac_tick);
  r->net = h.net;
  r->stage = h.stage;
  r->ok = h.body && json_parse(&r->j, h.body, h.len, ac_tok,
                               sizeof(ac_tok) / sizeof(ac_tok[0])) &&
          ac_tok[0].type == JSON_OBJ;
  return r->status;
}

static int ac_error_is(const AcReply *r, const char *code) {
  return r->ok && json_is(&r->j, json_get(&r->j, 0, "error"), code);
}

static void ac_field(const AcReply *r, const char *key, char *out, u32 max) {
  out[0] = 0;
  if (r->ok)
    json_str(&r->j, json_get(&r->j, 0, key), out, max);
}

/* A short reason for a failed exchange, for the third message line. */
static void ac_why(const AcReply *r, char *out) {
  HttpReply h;
  char code[40];
  h.net = r->net;
  h.status = r->status;
  http_why(&h, out);
  ac_field(r, "error", code, sizeof(code));
  if (r->status && code[0])
    ac_cpy(ac_cpy(out + ac_len(out), ": "), code);
}

/* The three message lines of the main screen's top, the first in a colour. */
static char ac_msg[3][96];
static Color ac_msg_color;

static void ac_say(Color c, const char *a, const char *a2, const char *b,
                   const char *d) {
  ac_msg_color = c;
  ac_cpy(ac_cpy(ac_msg[0], a), a2 ? a2 : "");
  ac_cpy(ac_msg[1], b);
  ac_cpy(ac_msg[2], d);
}

static void ac_say_state(void) {
  if (!ac_token[0])
    ac_say(COLOR_WHITE, L(STR_AC_NONE_L1), 0, L(STR_AC_NONE_L2),
           L(STR_AC_NONE_L3));
  else if (ac_user[0])
    ac_say(COLOR_WHITE, L(STR_AC_SIGNED_IN), ac_user, L(STR_AC_LINKED_L2),
           "");
  else
    ac_say(COLOR_WHITE, L(STR_AC_LINKED_L2), 0, "", "");
}

static void ac_top_base(void) {
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  status_bar_draw();
}

static void ac_top(const char *hint) {
  ac_top_base();
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 32, 96, TOP_SCREEN_HEIGHT,
          ASSET_ICON_USER_64, 0, COLOR_WHITE);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 130, TOP_SCREEN_HEIGHT,
              L(STR_AC_TITLE), COLOR_WHITE, COLOR_HM_BG_BOT, &ui_title);
  for (int i = 0; i < 3; i++)
    ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 164 + i * 19,
                    TOP_SCREEN_HEIGHT, ac_msg[i], TOP_SCREEN_WIDTH - 24,
                    i ? COLOR_HM_TEXT2 : ac_msg_color, COLOR_HM_BG_BOT,
                    &ui_font);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 224, TOP_SCREEN_HEIGHT, hint,
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

/* The busy screen: a title and a line on the bottom, the spinner under
 * them. Drawn and presented before the network command, so it is the frame
 * the core leaves on screen; ac_spin then only redraws the spinner. */
#define SPIN_Y 136

static void ac_spin(u32 ms) {
  static const s8 dx[8] = {0, 14, 20, 14, 0, -14, -20, -14};
  static const s8 dy[8] = {-20, -14, 0, 14, 20, 14, 0, -14};
  static const Color dim = {0x44, 0x44, 0x48};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT, cx = BOT_SCREEN_WIDTH / 2;
  int lit = (int)(ms / 100u) % 8;
  ui_wallpaper_rect(fb, cx - 26, SPIN_Y - 26, 52, 52, sh);
  for (int i = 0; i < 8; i++) {
    int age = (lit - i + 8) % 8;
    Color c = age == 0 ? g_accent : age < 3 ? COLOR_HM_TEXT2 : dim;
    draw_filled_round_rect(fb, cx + dx[i] - 3, SPIN_Y + dy[i] - 3, 7, 7, 3, sh,
                           c);
  }
  screen_present_bottom();
}

static void ac_busy(const char *title, const char *line) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT, cx = BOT_SCREEN_WIDTH / 2;
  ac_say(COLOR_WHITE, line, 0, "", "");
  ac_top("");
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  ui_text_mid_fit(fb, cx, 34, sh, title, BOT_SCREEN_WIDTH - 24, COLOR_WHITE,
                  COLOR_HM_BG_TOP, &ui_bold);
  ui_text_mid_fit(fb, cx, 64, sh, line, BOT_SCREEN_WIDTH - 24, COLOR_HM_TEXT2,
                  COLOR_HM_BG_TOP, &ui_font);
  ui_text_mid(fb, cx, 214, sh, L(STR_AC_PLEASE_WAIT), COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  ac_tick = ac_spin;
  ac_spin(0);
}

/* The joined network the core keeps up, joined first when it is not. 0, with
 * the reason in `why`, when there is none. */
static int ac_online(const char *title, char *why, int size) {
  static char ssid[33];
  if (http_online())
    return 1;
  ac_busy(title, L(STR_AC_JOINING));
  return wifi_net_join(ssid, sizeof(ssid), why, size, ac_spin);
}

/* A code from POST /v1/device/code, and when it came. */
typedef struct {
  char device[AC_TOKEN_LEN + 1];
  char user[AC_CODE_LEN + 1];
  char uri[64];       /* where to enter it, without https:// */
  char qr[128];       /* the address the QR code opens */
  u32 interval, expires;
  u32 t0;
} AcCode;

static int ac_new_code(AcCode *c, const char *name, int create,
                       const char *title) {
  static char form[128], full[96];
  AcReply r;
  ac_busy(title, L(STR_AC_CONNECTING));
  form[0] = 0;
  char *p = ac_form(form + 1, "client_id", AC_CLIENT);
  ac_form(p, "name", name);
  int st = ac_call("POST", "/v1/device/code", form + 1, 0, &r);
  if (st == 200 && r.ok) {
    ac_field(&r, "device_code", c->device, sizeof(c->device));
    ac_field(&r, "user_code", c->user, sizeof(c->user));
    ac_field(&r, "verification_uri", full, sizeof(full));
    c->interval = (u32)json_int(&r.j, json_get(&r.j, 0, "interval"), 5);
    c->expires = (u32)json_int(&r.j, json_get(&r.j, 0, "expires_in"), 600);
    if (c->interval < 1u || c->interval > 60u)
      c->interval = 5;
    if (c->expires < 30u || c->expires > 3600u)
      c->expires = 600;
    if (ac_token_ok(c->device) && ac_len(c->user) == AC_CODE_LEN) {
      if (!ac_kv(full, "https://", c->uri, sizeof(c->uri)) || !c->uri[0])
        ac_cpy(c->uri, ACCOUNT_SITE "/link");
      /* Signing up with next= comes back to /link with the code filled in.
       * The code is A-Z, 2-9 and a dash: nothing to encode. */
      if (create) {
        p = ac_cpy(c->qr, "https://" ACCOUNT_SITE
                          "/signup?next=%2Flink%3Fcode%3D");
        ac_cpy(c->uri, ACCOUNT_SITE "/signup");
      } else {
        p = ac_cpy(ac_cpy(ac_cpy(c->qr, "https://"), c->uri), "?code=");
      }
      ac_cpy(p, c->user);
      c->t0 = timer_ticks();
      return 1;
    }
  }
  ac_why(&r, full);
  ac_say(COLOR_RED, L(st == 429 ? STR_AC_BUSY_SRV : STR_AC_NO_SERVER), 0,
         L(STR_AC_TRY_LATER), full);
  return 0;
}

/* The code screen. Top: where to go and the code; bottom: the QR code. */
static char ac_status[64];
static Color ac_status_color;

static void ac_countdown(const AcCode *c) {
  static char t[48];
  u32 ms = timer_us_since(c->t0) / 1000u, all = c->expires * 1000u;
  u32 left = ms < all ? (all - ms + 999u) / 1000u : 0;
  char *p = ac_cpy(t, L(STR_AC_EXPIRES));
  p = ac_num(p, left / 60u);
  *p++ = ':';
  *p++ = (char)('0' + left % 60u / 10u);
  *p++ = (char)('0' + left % 10u);
  *p = 0;
  ui_wallpaper_rect(VRAM_TOP_LA, 0, 162, TOP_SCREEN_WIDTH, 46,
                    TOP_SCREEN_HEIGHT);
  ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 164, TOP_SCREEN_HEIGHT,
                  ac_status, TOP_SCREEN_WIDTH - 24, ac_status_color,
                  COLOR_HM_BG_BOT, &ui_font);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 188, TOP_SCREEN_HEIGHT, t,
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

static void ac_code_top(const AcCode *c, int create) {
  const int sh = TOP_SCREEN_HEIGHT, cx = TOP_SCREEN_WIDTH / 2;
  const int bw = 220, bh = 50, bx = cx - bw / 2, by = 104;
  ac_top_base();
  ui_text_mid_fit(VRAM_TOP_LA, cx, 28, sh,
                  L(create ? STR_AC_CREATE_AT : STR_AC_GO_TO),
                  TOP_SCREEN_WIDTH - 24, COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
                  &ui_font);
  ui_text_mid_fit(VRAM_TOP_LA, cx, 50, sh, c->uri, TOP_SCREEN_WIDTH - 24,
                  COLOR_WHITE, COLOR_HM_BG_BOT, &ui_bold);
  ui_text_mid_fit(VRAM_TOP_LA, cx, 76, sh,
                  L(create ? STR_AC_THEN_LINK : STR_AC_ENTER),
                  TOP_SCREEN_WIDTH - 24, COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
                  &ui_font);
  draw_gradient_round_rect(VRAM_TOP_LA, bx, by, bw, bh, 12, sh,
                           COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  draw_round_ring(VRAM_TOP_LA, bx - 3, by - 3, bw + 6, bh + 6, 15, 3, sh,
                  g_accent);
  ui_text_mid(VRAM_TOP_LA, cx, by + (bh - ui_th(&ui_title)) / 2, sh, c->user,
              COLOR_WHITE, COLOR_HM_SLOT, &ui_title);
  ui_text_mid(VRAM_TOP_LA, cx, 224, sh, L(STR_AC_CANCEL_HINT), COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  ac_countdown(c);
}

static void ac_code_bottom(const AcCode *c) {
  static Qr q;
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  int n = qr_make(&q, c->qr, (u32)ac_len(c->qr));
  if (n) {
    /* Four light modules around it, as the standard asks. */
    int s = 196 / (n + 8), w = (n + 8) * s;
    int x0 = (BOT_SCREEN_WIDTH - w) / 2, y0 = 10;
    draw_filled_round_rect(fb, x0, y0, w, w, 8, sh, COLOR_WHITE);
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n;) {
        int e = x;
        while (e < n && qr_at(&q, e, y))
          e++;
        if (e > x)
          draw_filled_rect(fb, x0 + (4 + x) * s, y0 + (4 + y) * s,
                           (e - x) * s, s, sh, COLOR_BLACK);
        x = e > x ? e : x + 1;
      }
    ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, y0 + w + 8, sh, L(STR_AC_SCAN),
                    BOT_SCREEN_WIDTH - 24, COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
                    &ui_small);
  }
  screen_present_bottom();
}

static const AcCode *ac_shown;
static u32 ac_shown_s;

/* While a poll runs: the countdown, if a second went by. */
static void ac_code_tick(u32 ms) {
  u32 s = timer_us_since(ac_shown->t0) / 1000000u;
  (void)ms;
  if (s != ac_shown_s) {
    ac_shown_s = s;
    ac_countdown(ac_shown);
  }
}

enum { POLL_CANCEL = 0, POLL_OK, POLL_FAIL, POLL_EXPIRED };

static void ac_set_status(StringId id, Color c) {
  ac_cpy(ac_status, L(id));
  ac_status_color = c;
}

/* Polls POST /v1/device/token every `interval` seconds until the person
 * approves or denies on the website, the code expires, or B cancels. The
 * token goes into `token`. */
static int ac_poll(AcCode *c, int create, char *token) {
  static char form[192];
  AcReply r;
  u32 next = c->interval * 1000u;
  ac_set_status(STR_AC_WAITING, COLOR_WHITE);
  ac_shown = c;
  ac_shown_s = 0xFFFFFFFFu;
  ac_code_top(c, create);
  ac_code_bottom(c);
  for (;;) {
    u32 k = get_keys_down();
    if (k & BUTTON_B)
      return POLL_CANCEL;
    u32 ms = timer_us_since(c->t0) / 1000u;
    if (ms >= c->expires * 1000u)
      return POLL_EXPIRED;
    if (ms >= next) {
      form[0] = 0;
      char *p = ac_form(form + 1, "grant_type",
                        "urn:ietf:params:oauth:grant-type:device_code");
      p = ac_form(p, "client_id", AC_CLIENT);
      ac_form(p, "device_code", c->device);
      ac_tick = ac_code_tick;
      int st = ac_call("POST", "/v1/device/token", form + 1, 0, &r);
      ac_wipe(form, sizeof(form));
      next = timer_us_since(c->t0) / 1000u + c->interval * 1000u;
      if (st == 200 && r.ok) {
        ac_field(&r, "access_token", token, AC_TOKEN_LEN + 1);
        if (ac_token_ok(token))
          return POLL_OK;
        ac_say(COLOR_RED, L(STR_AC_NO_SERVER), 0, L(STR_AC_TRY_LATER),
               L(STR_AC_E_REPLY));
        return POLL_FAIL;
      }
      if (ac_error_is(&r, "authorization_pending")) {
        ac_set_status(STR_AC_WAITING, COLOR_WHITE);
      } else if (ac_error_is(&r, "slow_down")) {
        c->interval += 5u;
        next += 5000u;
      } else if (ac_error_is(&r, "access_denied")) {
        ac_say(COLOR_ORANGE, L(STR_AC_DENIED), 0, L(STR_AC_DENIED2), "");
        return POLL_FAIL;
      } else if (ac_error_is(&r, "expired_token")) {
        return POLL_EXPIRED;
      } else if (st && st < 500) {
        static char why[96];
        ac_why(&r, why);
        ac_say(COLOR_RED, L(STR_AC_NO_SERVER), 0, L(STR_AC_TRY_LATER), why);
        return POLL_FAIL;
      } else {
        /* No reply, or a server error: worth another try. */
        ac_set_status(STR_AC_RETRYING, COLOR_ORANGE);
        if (!st && !wifi_online()) {
          static char why[96];
          if (!ac_online(L(create ? STR_AC_CREATE : STR_AC_LINK), why,
                         sizeof(why))) {
            ac_say(COLOR_RED, L(STR_AC_NO_NET), 0, why, "");
            return POLL_FAIL;
          }
          ac_shown_s = 0xFFFFFFFFu;
          ac_code_top(c, create);
          ac_code_bottom(c);
        }
      }
      ac_shown_s = 0xFFFFFFFFu;
    }
    ac_code_tick(0);
    ui_idle();
  }
}

/* GET /v1/me with the token: the username into `user`. Returns the HTTP
 * status (0 for none). */
static int ac_me(const char *token, char *user, u32 max, AcReply *r) {
  char keep[AC_TOKEN_LEN + 1];
  ac_cpy(keep, ac_token);
  ac_cpy(ac_token, token);
  int st = ac_call("GET", "/v1/me", 0, 1, r);
  ac_cpy(ac_token, keep);
  ac_wipe(keep, sizeof(keep));
  user[0] = 0;
  if (st == 200)
    ac_field(r, "username", user, max);
  return st;
}

/* Links the console: a code, the code screen until it is approved, then the
 * username, and the token saved. `create` sends the person to sign up
 * first. */
static void ac_link(const char *owner, int create) {
  static AcCode c;
  static char name[AC_NAME_MAX + 1], token[AC_TOKEN_LEN + 1], user[24],
      why[96];
  const char *title = L(create ? STR_AC_CREATE : STR_AC_LINK);
  AcReply r;
  ac_console_name(owner, name);
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  ac_busy(title, L(STR_AC_CONNECTING));
  if (!ac_online(title, why, sizeof(why))) {
    ac_say(COLOR_RED, L(STR_AC_NO_NET), 0, why, "");
    return;
  }
  for (int codes = 0;; codes++) {
    if (!ac_new_code(&c, name, create, title))
      return;
    int res = ac_poll(&c, create, token);
    ac_wipe(c.device, sizeof(c.device));
    if (res == POLL_OK)
      break;
    if (res == POLL_CANCEL) {
      ac_say_state();
      return;
    }
    if (res == POLL_FAIL)
      return;
    if (codes >= 5) { /* an hour of codes nobody entered */
      ac_say(COLOR_ORANGE, L(STR_AC_EXPIRED), 0, L(STR_AC_TRY_LATER), "");
      return;
    }
  }
  ac_busy(title, L(STR_AC_FINISHING));
  ac_me(token, user, sizeof(user), &r);
  if (!ac_store(token, user)) {
    ac_wipe(token, sizeof(token));
    ac_say(COLOR_RED, L(STR_AC_SD_FAIL), 0, L(STR_AC_TRY_LATER), "");
    return;
  }
  ac_wipe(token, sizeof(token));
  ac_say(COLOR_AURORA, L(STR_AC_LINK_DONE), 0, "", "");
  if (user[0])
    ac_cpy(ac_cpy(ac_msg[1], L(STR_AC_SIGNED_IN)), user);
}

/* GET /v1/me: refreshes the username, or finds the console unlinked. */
static void ac_check(void) {
  static char user[24], why[96];
  const char *title = L(STR_AC_CHECK);
  AcReply r;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  ac_busy(title, L(STR_AC_CONNECTING));
  if (!ac_online(title, why, sizeof(why))) {
    ac_say(COLOR_RED, L(STR_AC_NO_NET), 0, why, "");
    return;
  }
  ac_busy(title, L(STR_AC_CONNECTING));
  int st = ac_me(ac_token, user, sizeof(user), &r);
  if (st == 200 && user[0]) {
    int same = 1;
    for (int i = 0; same && (user[i] || ac_user[i]); i++)
      same = user[i] == ac_user[i];
    if (!same && !ac_store(ac_token, user)) {
      ac_say(COLOR_RED, L(STR_AC_SD_FAIL), 0, "", "");
      return;
    }
    ac_say(COLOR_AURORA, L(STR_AC_SIGNED_IN), user, L(STR_AC_LINKED_L2),
           L(STR_AC_CHECKED));
  } else if (st == 401 && ac_error_is(&r, "invalid_token")) {
    account_forget();
    ac_say(COLOR_ORANGE, L(STR_AC_REVOKED), 0, L(STR_AC_REVOKED2), "");
  } else {
    ac_why(&r, why);
    ac_say(COLOR_RED, L(st == 429 ? STR_AC_BUSY_SRV : STR_AC_NO_SERVER), 0,
           L(STR_AC_TRY_LATER), why);
  }
}

#define ROW_X    12
#define ROW_W    (BOT_SCREEN_WIDTH - 24)
#define ROW_H    34
#define ROW_STEP 42
#define ROW_Y0   8
#define AC_ROWS  2

static AnimList ac_list = {.count = AC_ROWS, .visible = AC_ROWS,
                           .step = ROW_STEP};

/* Row 0 is the main action, filled with the accent colour. */
static void ac_paint(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  static const StringId rows[2][AC_ROWS] = {{STR_AC_LINK, STR_AC_CREATE},
                                           {STR_AC_CHECK, STR_AC_UNLINK}};
  static const u32 icons[2][AC_ROWS] = {
      {ASSET_ICON_USER_32, ASSET_ICON_USER_32},
      {ASSET_ICON_GLOBE_32, ASSET_ICON_CROSS_32}};
  int linked = ac_token[0] != 0;
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  for (int i = 0; i < AC_ROWS; i++) {
    int y = ROW_Y0 + i * ROW_STEP;
    const Font *f = i ? &ui_font : &ui_bold;
    if (i)
      draw_gradient_round_rect(fb, ROW_X, y, ROW_W, ROW_H, 8, sh,
                               COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
    else
      draw_filled_round_rect(fb, ROW_X, y, ROW_W, ROW_H, 8, sh, g_accent);
    ui_icon(fb, ROW_X + 6, y, ROW_H, sh, icons[linked][i], 0, COLOR_WHITE);
    ui_text(fb, ROW_X + 44, y + (ROW_H - ui_th(f)) / 2, sh, L(rows[linked][i]),
            COLOR_WHITE, i ? COLOR_HM_SLOT : g_accent, f);
  }
  ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, 112, sh,
                  L(linked ? STR_AC_SITE_LINKED : STR_AC_SITE), ROW_W,
                  COLOR_HM_TEXT2, COLOR_HM_BG_TOP, &ui_small);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, 130, sh, ACCOUNT_SITE, COLOR_WHITE,
              COLOR_HM_BG_TOP, &ui_font);
  draw_round_ring(fb, ROW_X - 3, ROW_Y0 + anim_px(&ac_list.ring) - 3,
                  ROW_W + 6, ROW_H + 6, 11, 4, sh, g_accent);
  screen_present_bottom();
}

static void ac_unlink(void) {
  if (!fv_confirm(L(STR_AC_UNLINK_Q), L(STR_AC_UNLINK_SUB), L(STR_YES),
                  L(STR_NOT_NOW)))
    return;
  if (account_forget())
    ac_say(COLOR_WHITE, L(STR_AC_UNLINKED), 0, L(STR_AC_REVOKE_WEB),
           ACCOUNT_SITE);
  else
    ac_say(COLOR_RED, L(STR_AC_SD_FAIL), 0, "", "");
}

void account_screen(const char *owner) {
  int sel = 0;
  u32 frame_at = 0;
  account_load();
  ac_say_state();
  ac_top(L(STR_AC_HINT));
  anim_list_jump(&ac_list, sel);
  ac_paint();
  for (;;) {
    u32 k = get_keys_down();
    int prev = sel, activate = 0, tx, ty;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < AC_ROWS - 1)
      sel++;
    if (touch_tap(&tx, &ty))
      for (int i = 0; i < AC_ROWS; i++)
        if (touch_in(tx, ty, ROW_X - 3, ROW_Y0 + i * ROW_STEP - 3, ROW_W + 6,
                     ROW_H + 6)) {
          sel = i;
          activate = 1;
        }
    if (sel != prev) {
      anim_list_to(&ac_list, sel);
      if (!anim_list_moving(&ac_list))
        ac_paint();
    }
    if ((k & BUTTON_A) || activate) {
      int linked = ac_token[0] != 0, slid = 1;
      if (!linked)
        ac_link(owner, sel == 1);
      else if (sel == 0)
        ac_check();
      else {
        ac_unlink();
        slid = 0;
      }
      wifi_direct_off();
      if (slid)
        anim_transition(ANIM_POP, ANIM_BOTH);
      if ((ac_token[0] != 0) != linked)
        sel = 0;
      ac_top(L(STR_AC_HINT));
      anim_list_jump(&ac_list, sel);
      ac_paint();
    }
    if (k & BUTTON_B)
      return;
    int ms = anim_frame(&frame_at);
    if (ms && anim_list_step(&ac_list, ms))
      ac_paint();
    ui_idle();
  }
}

// Net: the network API. Joins the Wi-Fi network saved in Settings > Wi-Fi,
// then fetches a page, pings a host and downloads a file into the app's data
// folder (SD:/Aurora/Apps/C/Net/). The bottom screen shows a spinner while a
// call waits, through net_on_wait().
//
// A: fetch the page   Y: ping   X: download   START: quit
#include <aurora_app.h>

#include <stdio.h>
#include <string.h>

#define PAGE "http://example.com/"
#define HOST "example.com"

static const char *doing = "";

static void bottom(const char *line, uint32_t ms) {
  gfx_clear(SCREEN_BOTTOM, RGB(24, 26, 32));
  gfx_print(SCREEN_BOTTOM, 16, 16, FONT_BOLD, COLOR_WHITE, "%s", line);
  if (ms) {
    int a = (int)(ms / 100u) % 8;
    for (int i = 0; i < 8; i++)
      gfx_circle(SCREEN_BOTTOM, 160 + (i % 4 - 2) * 18 + 9,
                 120 + (i / 4) * 18 - 9, 6,
                 i == a ? COLOR_AURORA : RGB(70, 74, 84));
    gfx_print(SCREEN_BOTTOM, 16, 200, FONT_SMALL, COLOR_GRAY, "%u.%u s",
              (unsigned)(ms / 1000u), (unsigned)(ms / 100u % 10u));
  }
  gfx_present();
}

static void waiting(uint32_t ms) { bottom(doing, ms); }

static void download_progress(size_t done, size_t total) {
  char line[64];
  if (total)
    snprintf(line, sizeof(line), "Downloaded %u of %u KB",
             (unsigned)(done / 1024u), (unsigned)(total / 1024u));
  else
    snprintf(line, sizeof(line), "Downloaded %u KB", (unsigned)(done / 1024u));
  bottom(line, 0);
}

static void fetch(void) {
  uint32_t t0 = sys_millis();
  doing = "Fetching " PAGE;
  HttpResponse *r = http_get(PAGE);
  if (!r) {
    printf("Fetch failed: %s\n\n", net_error());
    return;
  }
  char type[64] = "?";
  http_header(r, "Content-Type", type, sizeof(type));
  printf("HTTP %d, %u bytes, %s, in %u ms\n", r->status,
         (unsigned)r->length, type, (unsigned)(sys_millis() - t0));
  // The first lines of the page, as text.
  int lines = 0;
  for (const char *p = r->body; *p && lines < 8; lines++) {
    const char *e = strchr(p, '\n');
    int n = e ? (int)(e - p) : (int)strlen(p);
    printf("  %.*s\n", n > 46 ? 46 : n, p);
    p += n + (e ? 1 : 0);
  }
  printf("\n");
  http_free(r);
}

static void ping(void) {
  char ip[16];
  doing = "Pinging " HOST;
  if (!net_resolve(HOST, ip)) {
    printf("Lookup failed: %s\n\n", net_error());
    return;
  }
  int ms = net_ping(ip);
  if (ms < 0)
    printf("Ping %s (%s) failed: %s\n\n", HOST, ip, net_error());
  else
    printf("Ping %s (%s): %d ms\n\n", HOST, ip, ms);
}

static void download(void) {
  char path[300];
  snprintf(path, sizeof(path), "%s/example.html", app_dir());
  doing = "Downloading";
  if (http_download(PAGE, path, download_progress))
    printf("Saved %s\n\n", path);
  else
    printf("Download failed: %s\n\n", net_error());
}

int main(void) {
  printf("Net: the AuroraOS network API\n\n");
  if (!net_available()) {
    printf("%s\n\nPress START to quit.\n", net_error());
    hid_wait(KEY_START);
    return 0;
  }
  net_on_wait(waiting);
  doing = "Joining the Wi-Fi network";
  if (!net_connect()) {
    printf("Could not connect: %s\n\nPress START to quit.\n", net_error());
    bottom("Not connected", 0);
    hid_wait(KEY_START);
    return 0;
  }
  printf("Connected. This console is %s\n\n", net_address());
  printf("A: fetch %s\nY: ping %s\nX: download the page\nSTART: quit\n\n",
         PAGE, HOST);
  bottom("Ready", 0);
  while (app_loop()) {
    uint32_t k = hid_keys_down();
    if (k & KEY_A)
      fetch();
    if (k & KEY_Y)
      ping();
    if (k & KEY_X)
      download();
    if (k & (KEY_A | KEY_X | KEY_Y))
      bottom("Ready", 0);
    if (k & KEY_START)
      break;
  }
  return 0;
}

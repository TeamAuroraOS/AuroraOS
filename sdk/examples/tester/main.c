/* Tester: an on-device regression suite for the AuroraOS C SDK and Wi-Fi.
 *
 * The local tests need no network. The network tests use the saved network
 * and plain HTTP, so they can be run after Settings > Wi-Fi has connected.
 *
 * A: run all tests again
 * X: run network tests
 * Y: exercise input and timing
 * START: quit
 */
#include <aurora_app.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int nanosleep(const struct timespec *req, struct timespec *rem);

#define TEST_HOST "example.com"
#define TEST_URL "http://example.com/"

static int passed;
static int failed;
static int skipped;
static const char *waiting_for;

static void result(const char *name, bool ok) {
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok)
    passed++;
  else
    failed++;
}

static void skip_test(const char *name, const char *why) {
  printf("[SKIP] %s (%s)\n", name, why);
  skipped++;
}

static bool bytes_equal(const void *a, const void *b, size_t n) {
  return memcmp(a, b, n) == 0;
}

static void local_tests(void) {
  char path[320];
  char renamed[320];
  const char text[] = "AuroraOS SDK tester\n0123456789\n";
  char buf[64];
  size_t size = 0;
  FILE *f;
  long pos;
  struct timespec req;

  printf("\n=== Local SDK tests ===\n");
  result("SD card available", fs_available());
  result("app data directory exists", fs_is_dir(app_dir()));
  snprintf(path, sizeof(path), "%s/local.txt", app_dir());
  snprintf(renamed, sizeof(renamed), "%s/local-renamed.txt", app_dir());

  result("fs_write_file", fs_write_file(path, text, sizeof(text) - 1));
  void *loaded = fs_read_file(path, &size);
  result("fs_read_file contents", loaded && size == sizeof(text) - 1 &&
                                  bytes_equal(loaded, text, size));
  free(loaded);

  FsFile stream = {-1};
  bool stream_ok = fs_open(&stream, path, "rb");
  if (stream_ok) {
    memset(buf, 0, sizeof(buf));
    stream_ok = fs_read(&stream, buf, sizeof(text) - 1) ==
                    (int)(sizeof(text) - 1) &&
                bytes_equal(buf, text, sizeof(text) - 1) &&
                fs_seek(&stream, 0, SEEK_SET) == 0 && fs_close(&stream);
  }
  result("streaming file API", stream_ok);

  f = fopen(path, "rb+");
  result("fopen app data file", f != NULL);
  if (f) {
    pos = fseek(f, 5, SEEK_SET);
    bool set_ok = pos == 0 && ftell(f) == 5;
    pos = fseek(f, 2, SEEK_CUR);
    bool cur_ok = pos == 0 && ftell(f) == 7;
    pos = fseek(f, -2, SEEK_END);
    bool end_ok = pos == 0 && ftell(f) == (long)size - 2;
    errno = 0;
    pos = fseek(f, 0, 12345);
    bool invalid_ok = pos != 0 && errno == EINVAL;
    fclose(f);
    result("SEEK_SET", set_ok);
    result("SEEK_CUR", cur_ok);
    result("SEEK_END", end_ok);
    result("invalid seek origin returns EINVAL", invalid_ok);
  } else {
    skip_test("seek tests", "file could not be opened");
  }

  memset(buf, 0, sizeof(buf));
  f = fopen(path, "rb");
  bool stdio_ok = f && fread(buf, 1, sizeof(text) - 1, f) == sizeof(text) - 1 &&
                  bytes_equal(buf, text, sizeof(text) - 1);
  if (f)
    fclose(f);
  result("stdio read/write bridge", stdio_ok);

  result("rename", rename(path, renamed) == 0 && fs_exists(renamed));
  result("remove", remove(renamed) == 0 && !fs_exists(renamed));

  req.tv_sec = 0;
  req.tv_nsec = 1000000;
  uint32_t before = sys_millis();
  result("nanosleep valid request", nanosleep(&req, NULL) == 0);
  result("nanosleep waited", sys_millis() >= before);
  req.tv_sec = -1;
  req.tv_nsec = 0;
  errno = 0;
  result("nanosleep negative seconds returns EINVAL",
         nanosleep(&req, NULL) != 0 && errno == EINVAL);
  req.tv_sec = 0;
  req.tv_nsec = 1000000000L;
  errno = 0;
  result("nanosleep invalid nanoseconds returns EINVAL",
         nanosleep(&req, NULL) != 0 && errno == EINVAL);

  SysTime now;
  result("system clock", sys_time(&now) && now.month >= 1 && now.month <= 12 &&
                              now.day >= 1 && now.day <= 31);
  result("app launched through Home Menu", app_from_home());
  printf("App: %s\nData: %s\n", app_path(), app_dir());
}

static void wait_screen(uint32_t ms) {
  gfx_clear(SCREEN_BOTTOM, RGB(18, 22, 30));
  gfx_print(SCREEN_BOTTOM, 16, 18, FONT_BOLD, COLOR_WHITE, "%s", waiting_for);
  gfx_print(SCREEN_BOTTOM, 16, 48, FONT_SMALL, COLOR_GRAY, "Waiting: %u.%u s",
            (unsigned)(ms / 1000u), (unsigned)(ms / 100u % 10u));
  gfx_present();
}

static void download_progress(size_t done, size_t total) {
  gfx_clear(SCREEN_BOTTOM, RGB(18, 22, 30));
  gfx_print(SCREEN_BOTTOM, 16, 18, FONT_BOLD, COLOR_WHITE, "Downloading");
  if (total)
    gfx_print(SCREEN_BOTTOM, 16, 48, FONT_SMALL, COLOR_GRAY, "%u / %u KB",
              (unsigned)(done / 1024u), (unsigned)(total / 1024u));
  else
    gfx_print(SCREEN_BOTTOM, 16, 48, FONT_SMALL, COLOR_GRAY, "%u KB",
              (unsigned)(done / 1024u));
  gfx_present();
}

static void network_tests(void) {
  char ip[16];
  char path[320];
  char reply[256];
  size_t size = 0;
  const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  const char post_data[] = "tester=aurora";
  void *loaded;
  HttpResponse *response;

  printf("\n=== Network tests ===\n");
  if (!net_available()) {
    skip_test("Wi-Fi API available", net_error());
    skip_test("Wi-Fi connection and HTTP tests", "Wi-Fi is unavailable");
    return;
  }
  result("Wi-Fi API available", true);
  result("network error code starts clear", net_error_code() == AURORA_NET_OK);
  net_on_wait(wait_screen);
  waiting_for = "Joining saved Wi-Fi";
  bool connected = net_connect();
  result("join saved Wi-Fi", connected);
  if (!connected) {
    printf("Network error: %s\n", net_error());
    skip_test("remaining network tests", "join failed");
    net_on_wait(NULL);
    return;
  }

  result("network has an address", net_address()[0] != '\0');
  result("network remains connected", net_connected());
  result("DNS resolve", net_resolve(TEST_HOST, ip));
  if (ip[0])
    printf("Resolved %s -> %s\n", TEST_HOST, ip);

  int ping = net_ping(TEST_HOST);
  result("DNS/ICMP ping", ping >= 0);
  if (ping >= 0)
    printf("Ping: %d ms\n", ping);

  waiting_for = "HTTP GET";
  response = http_get(TEST_URL);
  result("HTTP GET returns a response", response != NULL);
  if (response) {
    char content_type[80] = "";
    bool body_ok = response->status > 0 && response->body &&
                   response->length > 0;
    result("HTTP response has status and body", body_ok);
    result("HTTP Content-Type header", http_header(
        response, "Content-Type", content_type, sizeof(content_type)));
    printf("GET status %d, %u bytes, type %s\n", response->status,
           (unsigned)response->length, content_type);
    http_free(response);
  }

  waiting_for = "HTTP POST";
  response = http_post(TEST_URL, "text/plain", post_data,
                       sizeof(post_data) - 1);
  result("HTTP POST returns a response", response != NULL);
  if (response) {
    result("HTTP POST has a valid status", response->status > 0);
    http_free(response);
  }

  char long_type[300];
  memset(long_type, 'x', sizeof(long_type) - 1);
  long_type[sizeof(long_type) - 1] = '\0';
  response = http_post(TEST_URL, long_type, post_data,
                       sizeof(post_data) - 1);
  result("oversize Content-Type is rejected", response == NULL &&
                                             strcmp(net_error(),
                                                    "the content type is too long") == 0);
  if (response)
    http_free(response);

  waiting_for = "HTTP request";
  response = http_request("HEAD", TEST_URL, "X-Aurora-Tester: 1\r\n",
                          NULL, 0);
  result("custom HTTP request returns a response", response != NULL);
  if (response)
    http_free(response);

  waiting_for = "TCP exchange";
  int received = net_tcp_exchange(TEST_HOST, 80, request, sizeof(request) - 1,
                                  reply, sizeof(reply) - 1);
  result("raw TCP exchange receives data", received > 0);
  if (received > 0) {
    reply[received < (int)sizeof(reply) - 1 ? received : (int)sizeof(reply) - 1] = 0;
    result("raw TCP exchange contains HTTP", strstr(reply, "HTTP/") != NULL);
  }

  snprintf(path, sizeof(path), "%s/network-download.html", app_dir());
  waiting_for = "HTTP download";
  bool downloaded = http_download(TEST_URL, path, download_progress);
  result("HTTP download succeeds", downloaded);
  if (downloaded) {
    loaded = fs_read_file(path, &size);
    result("download is readable", loaded && size > 0);
    free(loaded);
    remove(path);
  }
  net_on_wait(NULL);
}

static void input_test(void) {
  int x = 0, y = 0;
  printf("\n=== Input/timing test ===\n");
  printf("Press A, touch each bottom-screen corner, and move the circle pad.\n");
  printf("Press B when done (START also skips).\n");
  while (app_loop()) {
    uint32_t down = hid_keys_down();
    hid_cpad(&x, &y);
    gfx_clear(SCREEN_BOTTOM, RGB(20, 24, 32));
    gfx_print(SCREEN_BOTTOM, 12, 12, FONT_BOLD, COLOR_WHITE, "Input test");
    gfx_print(SCREEN_BOTTOM, 12, 42, FONT_SMALL, COLOR_GRAY,
              "A:%s  Touch:%s", down & KEY_A ? "down" : "wait",
              hid_touch_held() ? "held" : "none");
    gfx_print(SCREEN_BOTTOM, 12, 68, FONT_SMALL, COLOR_GRAY,
              "Touch: %d,%d", hid_touch_x(), hid_touch_y());
    gfx_print(SCREEN_BOTTOM, 12, 94, FONT_SMALL, COLOR_GRAY,
              "Circle pad: %d,%d", x, y);
    gfx_print(SCREEN_BOTTOM, 12, 142, FONT_SMALL, COLOR_YELLOW,
              "B = finish   START = skip");
    gfx_present();
    if (down & KEY_B)
      break;
    if (down & KEY_START) {
      skip_test("interactive input", "skipped");
      return;
    }
  }
  result("interactive input loop completed", true);
}

static void run_all(void) {
  passed = failed = skipped = 0;
  con_clear();
  printf("AuroraOS SDK TESTER %s\n", AURORA_SDK_VERSION);
  printf("Run A to execute the full suite.\n");
  local_tests();
  network_tests();
  printf("\nResults so far: %d passed, %d failed, %d skipped\n",
         passed, failed, skipped);
}

int main(void) {
  run_all();
  printf("\nA: rerun all  X: network  Y: input  START: quit\n");
  while (app_loop()) {
    uint32_t down = hid_keys_down();
    if (down & KEY_A)
      run_all();
    if (down & KEY_X) {
      con_clear();
      network_tests();
      printf("\nResults: %d passed, %d failed, %d skipped\n", passed, failed,
             skipped);
    }
    if (down & KEY_Y)
      input_test();
    if (down & KEY_START)
      break;
  }
  return 0;
}

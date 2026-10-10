// Hello: the C library's printf on the AuroraOS console.
#include <aurora_app.h>
#include <stdio.h>

int main(void) {
  printf("Hello from C on AuroraOS!\n\n");
  printf("App:    %s\n", app_path());
  printf("Data:   %s\n", app_dir());
  printf("User:   %s\n", sys_user_name());
  printf("Model:  %s 3DS\n", sys_is_new3ds() ? "New" : "Old");
  printf("\nPress A to quit.\n");
  hid_wait(KEY_A);
  return 0;
}

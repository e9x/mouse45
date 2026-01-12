#include <X11/keysym.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

void send_click(xcb_connection_t *c, uint8_t button) {
  xcb_test_fake_input(c, XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, XCB_NONE,
                      0, 0, 0);
  xcb_test_fake_input(c, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, XCB_NONE,
                      0, 0, 0);
  xcb_flush(c);
}

void send_key_tap(xcb_connection_t *c, xcb_keycode_t code) {
  xcb_test_fake_input(c, XCB_KEY_PRESS, code, XCB_CURRENT_TIME, XCB_NONE, 0, 0,
                      0);
  xcb_test_fake_input(c, XCB_KEY_RELEASE, code, XCB_CURRENT_TIME, XCB_NONE, 0,
                      0, 0);
  xcb_flush(c);
}

xcb_keycode_t get_keycode(xcb_connection_t *c, xcb_keysym_t target_sym) {
  const xcb_setup_t *setup = xcb_get_setup(c);
  xcb_get_keyboard_mapping_cookie_t cookie = xcb_get_keyboard_mapping(
      c, setup->min_keycode, setup->max_keycode - setup->min_keycode + 1);
  xcb_get_keyboard_mapping_reply_t *reply =
      xcb_get_keyboard_mapping_reply(c, cookie, NULL);

  if (!reply)
    return 0;

  xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(reply);
  int length = xcb_get_keyboard_mapping_keysyms_length(reply);
  int syms_per_code = reply->keysyms_per_keycode;

  xcb_keycode_t result = 0;
  for (int i = 0; i < length; i++) {
    if (syms[i] == target_sym) {
      result = setup->min_keycode + (i / syms_per_code);
      break;
    }
  }
  free(reply);
  return result;
}

xcb_connection_t *conn;
xcb_window_t root;

bool grab_key(xcb_keycode_t key) {
  xcb_void_cookie_t cookie =
      xcb_grab_key_checked(conn, 1, root, XCB_MOD_MASK_ANY, key,
                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

  xcb_generic_error_t *error = xcb_request_check(conn, cookie);
  if (error) {
    free(error);
    return false;
  }
  return true;
}

void ungrab_key(xcb_keycode_t key) {
  xcb_ungrab_key(conn, key, root, XCB_MOD_MASK_ANY);
}

atomic_bool bhop_enabled = true;
atomic_bool brackets_enabled = true;

void *bhop_thread_func(void *_) {
  xcb_connection_t *bhop_conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(bhop_conn))
    return NULL;

  xcb_keycode_t grave = get_keycode(bhop_conn, XK_grave);
  xcb_keycode_t space = get_keycode(bhop_conn, XK_space);

  while (true) {
    if (!atomic_load(&bhop_enabled)) {
      usleep(100000); // 100ms
      continue;
    }

    xcb_query_keymap_cookie_t cookie = xcb_query_keymap(bhop_conn);
    xcb_query_keymap_reply_t *reply =
        xcb_query_keymap_reply(bhop_conn, cookie, NULL);

    if (reply) {
      if ((reply->keys[grave / 8] & (1 << (grave % 8)))) {
        send_key_tap(bhop_conn, space);
        usleep(15000); // 15ms
      } else {
        usleep(10000); // 10ms
      }
      free(reply);
    }
  }
  return NULL;
}

xcb_keycode_t k_up;
xcb_keycode_t k_down;

void *terminal_input_func(void *_) {
  struct termios old_settings, new_settings;
  tcgetattr(STDIN_FILENO, &old_settings);
  new_settings = old_settings;
  new_settings.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &new_settings);

  while (true) {
    int c = getchar();

    if (c == 'b' || c == 'B') {
      bool expected = atomic_load(&bhop_enabled);
      atomic_store(&bhop_enabled, !expected);
      printf("\r[BHOP]: %s    ",
             !expected ? "\033[1;32mON \033[0m" : "\033[1;31mOFF\033[0m");
      fflush(stdout);
    }

    if (c == 's' || c == 'S') {
      bool active = atomic_load(&brackets_enabled);
      active = !active;
      atomic_store(&brackets_enabled, active);
      if (active) {
        grab_key(k_up);
        grab_key(k_down);
      } else {
        ungrab_key(k_up);
        ungrab_key(k_down);
      }
      xcb_flush(conn);
      printf("\r[MOUSE]: %s   ",
             active ? "\033[1;32mON \033[0m" : "\033[1;31mOFF\033[0m");
      fflush(stdout);
    }

    if (c == 'q' || c == 'Q') {
      tcsetattr(STDIN_FILENO, TCSANOW, &old_settings);
      printf("\nClosing...\n");
      exit(0);
    }
  }
  return NULL;
}

int main(void) {
  const char *session = getenv("XDG_SESSION_TYPE");
  if (session && strcmp(session, "wayland") == 0) {
    fprintf(stderr, "[FATAL] Wayland detected. Use X11.\n");
    return 1;
  }

  conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn))
    return 1;

  const xcb_setup_t *setup = xcb_get_setup(conn);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  xcb_screen_t *screen = iter.data;
  root = screen->root;

  k_up = get_keycode(conn, XK_bracketright);
  k_down = get_keycode(conn, XK_bracketleft);
  if (!grab_key(k_up) || !grab_key(k_down)) {
    fprintf(stderr,
            "[FATAL] Could not grab brackets. Close other apps using them.\n");
    return 1;
  }

  pthread_t t_bhop;
  if (pthread_create(&t_bhop, NULL, bhop_thread_func, NULL) != 0) {
    fprintf(stderr, "Failed to create bhop thread\n");
    return 1;
  }
  pthread_detach(t_bhop);

  pthread_t t_input;
  if (pthread_create(&t_input, NULL, terminal_input_func, NULL) != 0) {
    fprintf(stderr, "Failed to create input thread\n");
    return 1;
  }
  pthread_detach(t_input);
  // 45 chars:
  // --------------------------------------------
  // 17 chars:
  // mouse45 Controls:
  // 8 chars:
  // Toggles:
  printf("--------------mouse45 Controls:-------------\n");
  printf(" [ : Scroll Down         | Ctrl+[ : Back\n");
  printf(" ] : Scroll Up           | Ctrl+] : Forward\n");
  printf(" ` : Bhop\n");
  printf("------------------Toggles:------------------\n");
  printf(" b : Toggle Bhop\n");
  printf(" s : Toggle Mouse Scroll\n");
  printf(" q : Quit\n");
  xcb_generic_event_t *event;
  while ((event = xcb_wait_for_event(conn))) {
    if ((event->response_type & ~0x80) == XCB_KEY_PRESS) {
      xcb_key_press_event_t *kp = (xcb_key_press_event_t *)event;
      bool ctrl_held = kp->state & XCB_MOD_MASK_CONTROL;
      if (kp->detail == k_up) {
        if (ctrl_held)
          send_click(conn, 9);
        else
          send_click(conn, 4);
      } else if (kp->detail == k_down) {
        if (ctrl_held)
          send_click(conn, 8);
        else
          send_click(conn, 5);
      }
    }
    free(event);
  }
  return 0;
}
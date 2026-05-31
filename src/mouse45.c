#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <ncurses.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wordexp.h>

#define SCROLL_DELAY_US 35000 // 35ms
#define BHOP_DELAY_US 20000   // 20ms
#define F_SPAM_DELAY_US 50000 // 50ms
#define MAX_DEVICES 50
#define MAX_PATH_LEN 256
#define CONFIG_DIR "~/.config/mouse45"
#define CONFIG_FILE CONFIG_DIR "/config_v2.bin" 

#define CP_NORMAL 1
#define CP_GREEN 2
#define CP_RED 3
#define CP_HEADER 4
#define CP_HIGHLIGHT 5

typedef struct {
  bool bhop_enabled;
  bool scroll_enabled;
  bool f_spam_enabled; 
  char mouse_path[MAX_PATH_LEN];
  char kbd_path[MAX_PATH_LEN];
} AppSettings;

AppSettings settings = {.bhop_enabled = true,
                        .scroll_enabled = true,
                        .f_spam_enabled = false,
                        .mouse_path = {0},
                        .kbd_path = {0}};

int mouse_fd = -1;
int kbd_fd = -1;
int virt_fd = -1;
atomic_bool keep_running = true;

atomic_bool action_scroll_up = false;
atomic_bool action_scroll_down = false;
atomic_bool action_bhop = false;
atomic_bool action_f_spam = false; 

pthread_mutex_t uinput_lock = PTHREAD_MUTEX_INITIALIZER;

void init_ncurses_ui() {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(CP_NORMAL, -1, -1);
  init_pair(CP_GREEN, COLOR_GREEN, -1);
  init_pair(CP_RED, COLOR_RED, -1);
  init_pair(CP_HEADER, COLOR_CYAN, -1);
  init_pair(CP_HIGHLIGHT, COLOR_BLACK, COLOR_WHITE);
}

void get_expanded_path(const char *raw_path, char *buffer) {
  if (strncmp(raw_path, "~/", 2) == 0) {
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user) {
      snprintf(buffer, MAX_PATH_LEN, "/home/%s/%s", sudo_user, raw_path + 2);
      return;
    }
  }

  wordexp_t p;
  wordexp(raw_path, &p, 0);
  if (p.we_wordc > 0) {
    strncpy(buffer, p.we_wordv[0], MAX_PATH_LEN - 1);
    buffer[MAX_PATH_LEN - 1] = '\0';
  }
  wordfree(&p);
}

void ensure_config_dir_exists() {
  char dir_path[MAX_PATH_LEN];
  get_expanded_path(CONFIG_DIR, dir_path);
  mkdir(dir_path, 0755);
}

void save_settings() {
  ensure_config_dir_exists();
  char full_path[MAX_PATH_LEN];
  get_expanded_path(CONFIG_FILE, full_path);

  FILE *f = fopen(full_path, "wb");
  if (!f)
    return;

  fwrite(&settings.bhop_enabled, sizeof(bool), 1, f);
  fwrite(&settings.scroll_enabled, sizeof(bool), 1, f);
  fwrite(&settings.f_spam_enabled, sizeof(bool), 1, f);

  size_t m_len = strlen(settings.mouse_path);
  fwrite(&m_len, sizeof(size_t), 1, f);
  if (m_len > 0) {
    fwrite(settings.mouse_path, 1, m_len, f);
  }

  size_t k_len = strlen(settings.kbd_path);
  fwrite(&k_len, sizeof(size_t), 1, f);
  if (k_len > 0) {
    fwrite(settings.kbd_path, 1, k_len, f);
  }

  fclose(f);
}

bool load_settings() {
  char full_path[MAX_PATH_LEN];
  get_expanded_path(CONFIG_FILE, full_path);

  FILE *f = fopen(full_path, "rb");
  if (!f)
    return false;

  if (fread(&settings.bhop_enabled, sizeof(bool), 1, f) != 1) {
    fclose(f);
    return false;
  }

  if (fread(&settings.scroll_enabled, sizeof(bool), 1, f) != 1) {
    fclose(f);
    return false;
  }

  if (fread(&settings.f_spam_enabled, sizeof(bool), 1, f) != 1) {
    fclose(f);
    return false;
  }

  size_t m_len = 0;
  if (fread(&m_len, sizeof(size_t), 1, f) == 1) {
    if (m_len >= MAX_PATH_LEN)
      m_len = MAX_PATH_LEN - 1;
    if (m_len > 0) {
      fread(settings.mouse_path, 1, m_len, f);
    }
    settings.mouse_path[m_len] = '\0';
  }

  size_t k_len = 0;
  if (fread(&k_len, sizeof(size_t), 1, f) == 1) {
    if (k_len >= MAX_PATH_LEN)
      k_len = MAX_PATH_LEN - 1;
    if (k_len > 0) {
      fread(settings.kbd_path, 1, k_len, f);
    }
    settings.kbd_path[k_len] = '\0';
  }

  fclose(f);

  if (access(settings.mouse_path, F_OK) != 0 ||
      access(settings.kbd_path, F_OK) != 0) {
    return false;
  }

  return true;
}

int ends_with(const char *str, const char *suffix) {
  if (!str || !suffix)
    return 0;
  size_t len_str = strlen(str);
  size_t len_suffix = strlen(suffix);
  if (len_suffix > len_str)
    return 0;
  return strncmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
}

void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  struct input_event ie;
  memset(&ie, 0, sizeof(ie));
  ie.type = type;
  ie.code = code;
  ie.value = value;
  write(fd, &ie, sizeof(ie));
}

void wait_for_release(int fd, const char *device_name) {
  if (fd < 0)
    return;

  uint8_t key_b[KEY_MAX / 8 + 1];
  bool is_held = true;

  while (is_held && atomic_load(&keep_running)) {
    memset(key_b, 0, sizeof(key_b));

    if (ioctl(fd, EVIOCGKEY(sizeof(key_b)), key_b) < 0) {
      break;
    }

    is_held = false;
    for (int i = 0; i < sizeof(key_b); i++) {
      if (key_b[i] != 0) {
        is_held = true;
        break;
      }
    }

    if (is_held) {
      attron(COLOR_PAIR(CP_RED));
      mvprintw(13, 1, "WAITING FOR RELEASE: Please let go of %s keys...",
               device_name);
      attroff(COLOR_PAIR(CP_RED));
      refresh();
      usleep(100000);
    }
  }

  move(13, 0);
  clrtoeol();
  refresh();
}

void sync_device_state(int src_fd, int dst_fd) {
  uint8_t key_bitmask[KEY_MAX / 8 + 1];
  memset(key_bitmask, 0, sizeof(key_bitmask));

  if (ioctl(src_fd, EVIOCGKEY(sizeof(key_bitmask)), key_bitmask) < 0) {
    return;
  }

  pthread_mutex_lock(&uinput_lock);

  int buttons[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
  bool needed_sync = false;

  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
    int code = buttons[i];
    int byte_idx = code / 8;
    int bit_idx = code % 8;

    if ((key_bitmask[byte_idx] >> bit_idx) & 1) {
      emit(dst_fd, EV_KEY, code, 1);
      needed_sync = true;
    }
  }

  if (needed_sync) {
    emit(dst_fd, EV_SYN, SYN_REPORT, 0);
  }

  pthread_mutex_unlock(&uinput_lock);
}

void release_all_keys(int fd) {
  if (fd < 0)
    return;
  pthread_mutex_lock(&uinput_lock);
  for (int i = 0; i < 256; i++) {
    emit(fd, EV_KEY, i, 0);
  }
  emit(fd, EV_KEY, BTN_LEFT, 0);
  emit(fd, EV_KEY, BTN_RIGHT, 0);
  emit(fd, EV_KEY, BTN_MIDDLE, 0);
  emit(fd, EV_KEY, BTN_SIDE, 0);
  emit(fd, EV_KEY, BTN_EXTRA, 0);
  emit(fd, EV_SYN, SYN_REPORT, 0);
  pthread_mutex_unlock(&uinput_lock);
}

void draw_main_ui() {
  erase();
  attron(COLOR_PAIR(CP_NORMAL));

  attron(COLOR_PAIR(CP_HEADER));
  mvprintw(0, 0, " mouse45 running ");
  attroff(COLOR_PAIR(CP_HEADER));

  attron(COLOR_PAIR(CP_NORMAL));
  mvprintw(1, 0, "---------------------------------------------------------");

  mvprintw(3, 1, "[s] Scrolling  : ");
  if (settings.scroll_enabled) {
    attron(COLOR_PAIR(CP_GREEN));
    printw("ON ");
    attroff(COLOR_PAIR(CP_GREEN));
  } else {
    attron(COLOR_PAIR(CP_RED));
    printw("OFF");
    attroff(COLOR_PAIR(CP_RED));
  }
  attron(COLOR_PAIR(CP_NORMAL));
  printw(" (Side buttons)");

  mvprintw(4, 1, "[b] Bhop       : ");
  if (settings.bhop_enabled) {
    attron(COLOR_PAIR(CP_GREEN));
    printw("ON ");
    attroff(COLOR_PAIR(CP_GREEN));
  } else {
    attron(COLOR_PAIR(CP_RED));
    printw("OFF");
    attroff(COLOR_PAIR(CP_RED));
  }
  attron(COLOR_PAIR(CP_NORMAL));
  printw(" (Hold `)");

  mvprintw(5, 1, "[f] F Spam     : ");
  if (settings.f_spam_enabled) {
    attron(COLOR_PAIR(CP_GREEN));
    printw("ON ");
    attroff(COLOR_PAIR(CP_GREEN));
  } else {
    attron(COLOR_PAIR(CP_RED));
    printw("OFF");
    attroff(COLOR_PAIR(CP_RED));
  }
  attron(COLOR_PAIR(CP_NORMAL));
  printw(" (Hold J)");

  mvprintw(7, 0, "---------------------------------------------------------");
  mvprintw(8, 1, "BackBtn        : Scroll Down | Ctrl + BackBtn : Back");
  mvprintw(9, 1, "FwdBtn         : Scroll Up   | Ctrl + FwdBtn  : Forward");
  mvprintw(10, 0, "---------------------------------------------------------");
  mvprintw(12, 1, "[z] Reselect Devices | [q] Quit");

  attroff(COLOR_PAIR(CP_NORMAL));
  refresh();
}

int select_device(char *selected_path, size_t len, const char *error_msg,
                  const char *current_pref, const char *filter,
                  const char *title) {
  char *dev_names[MAX_DEVICES];
  int count = 0;
  struct dirent *entry;
  const char *dev_dir = "/dev/input/by-id/";

  DIR *dp = opendir(dev_dir);
  if (!dp) {
    return -1;
  }

  while ((entry = readdir(dp))) {
    if (ends_with(entry->d_name, filter)) {
      if (count < MAX_DEVICES) {
        dev_names[count] = strdup(entry->d_name);
        count++;
      }
    }
  }
  closedir(dp);

  if (count == 0) {
    return -1;
  }

  int highlight = 0;
  int ch = 0;
  bool selected = false;

  while (!selected && atomic_load(&keep_running)) {
    erase();
    attron(COLOR_PAIR(CP_NORMAL));

    if (error_msg && *error_msg) {
      attron(COLOR_PAIR(CP_RED));
      mvprintw(0, 0, "Error: %s", error_msg);
      attroff(COLOR_PAIR(CP_RED));
    }

    attron(COLOR_PAIR(CP_HEADER));
    mvprintw(2, 0, " Select %s (Arrow Keys / Enter) ", title);
    attroff(COLOR_PAIR(CP_HEADER));

    attron(COLOR_PAIR(CP_NORMAL));
    mvprintw(3, 0, " 'q' to go back/quit ");

    for (int i = 0; i < count; i++) {
      int y = 5 + i;
      if (i == highlight) {
        attron(COLOR_PAIR(CP_HIGHLIGHT));
      } else {
        attron(COLOR_PAIR(CP_NORMAL));
      }

      char marker[16] = "";
      if (current_pref && strstr(current_pref, dev_names[i])) {
        strcpy(marker, "[Current]");
      }

      mvprintw(y, 2, "%d) %s %s", i + 1, dev_names[i], marker);

      if (i == highlight) {
        attroff(COLOR_PAIR(CP_HIGHLIGHT));
      }
    }

    attroff(COLOR_PAIR(CP_NORMAL));
    refresh();

    ch = getch();
    switch (ch) {
    case KEY_UP:
      if (highlight > 0)
        highlight--;
      else
        highlight = count - 1;
      break;
    case KEY_DOWN:
      if (highlight < count - 1)
        highlight++;
      else
        highlight = 0;
      break;
    case 10: // enter
      selected = true;
      break;
    case 'q':
    case 'Q':
      for (int i = 0; i < count; i++)
        free(dev_names[i]);
      return -1;
    }
  }

  if (selected) {
    snprintf(selected_path, len, "%s%s", dev_dir, dev_names[highlight]);
  }

  for (int i = 0; i < count; i++) {
    free(dev_names[i]);
  }

  return selected ? 0 : -1;
}

void *output_thread(void *arg) {
  int sc_timer = 0;
  int bh_timer = 0;
  int f_timer = 0;
  
  while (atomic_load(&keep_running)) {
    bool up = atomic_load(&action_scroll_up);
    bool down = atomic_load(&action_scroll_down);
    bool hop = atomic_load(&action_bhop);
    bool f_spam = atomic_load(&action_f_spam);

    if (!up && !down && !hop && !f_spam) {
      usleep(10000);
      continue;
    }

    pthread_mutex_lock(&uinput_lock);

    if (up || down) {
      if (sc_timer <= 0) {
        emit(virt_fd, EV_REL, REL_WHEEL, up ? 1 : -1);
        emit(virt_fd, EV_SYN, SYN_REPORT, 0);
        sc_timer = SCROLL_DELAY_US;
      }
      sc_timer -= 5000;
    }

    if (hop) {
      if (bh_timer <= 0) {
        emit(virt_fd, EV_KEY, KEY_SPACE, 1);
        emit(virt_fd, EV_SYN, SYN_REPORT, 0);
        pthread_mutex_unlock(&uinput_lock);
        usleep(2000);
        pthread_mutex_lock(&uinput_lock);
        emit(virt_fd, EV_KEY, KEY_SPACE, 0);
        emit(virt_fd, EV_SYN, SYN_REPORT, 0);
        bh_timer = BHOP_DELAY_US;
      }
      bh_timer -= 5000;
    }

    if (f_spam) {
      if (f_timer <= 0) {
        //emit(virt_fd, EV_KEY, 33 /* KEY_F */, 1);
        emit(virt_fd, EV_KEY, 18 /* KEY_E */, 1);
	emit(virt_fd, EV_SYN, SYN_REPORT, 0);
        pthread_mutex_unlock(&uinput_lock);
        usleep(2000);
        pthread_mutex_lock(&uinput_lock);
        //emit(virt_fd, EV_KEY, 33 /* KEY_F */, 0);
        emit(virt_fd, EV_KEY, 18 /* KEY_E */, 0);
	emit(virt_fd, EV_SYN, SYN_REPORT, 0);
        f_timer = F_SPAM_DELAY_US;
      }
      f_timer -= 5000;
    }

    pthread_mutex_unlock(&uinput_lock);
    usleep(5000);
  }
  return NULL;
}

void cleanup(int signo) {
  atomic_store(&keep_running, false);

  if (virt_fd >= 0) {
    release_all_keys(virt_fd);
    ioctl(virt_fd, UI_DEV_DESTROY);
    close(virt_fd);
  }

  if (mouse_fd >= 0) {
    ioctl(mouse_fd, EVIOCGRAB, 0);
    close(mouse_fd);
  }
  if (kbd_fd >= 0) {
    ioctl(kbd_fd, EVIOCGRAB, 0);
    close(kbd_fd);
  }

  endwin();
  printf("byeee\n");
  exit(0);
}

int main(void) {
  init_ncurses_ui();

  signal(SIGINT, cleanup);
  signal(SIGTERM, cleanup);

  bool loaded = load_settings();
  
  // Keep action set to false at startup so it only spams when physically held
  atomic_store(&action_f_spam, false);

  virt_fd = open("/dev/uinput", O_WRONLY);
  if (virt_fd < 0) {
    endwin();
    perror("Failed to open /dev/uinput");
    return 1;
  }

  ioctl(virt_fd, UI_SET_EVBIT, EV_KEY);
  ioctl(virt_fd, UI_SET_EVBIT, EV_REL);
  ioctl(virt_fd, UI_SET_RELBIT, REL_X);
  ioctl(virt_fd, UI_SET_RELBIT, REL_Y);
  ioctl(virt_fd, UI_SET_RELBIT, REL_WHEEL);
  for (int i = 0; i < 256; i++)
    ioctl(virt_fd, UI_SET_KEYBIT, i);
  ioctl(virt_fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(virt_fd, UI_SET_KEYBIT, BTN_RIGHT);
  ioctl(virt_fd, UI_SET_KEYBIT, BTN_MIDDLE);
  ioctl(virt_fd, UI_SET_KEYBIT, BTN_SIDE);
  ioctl(virt_fd, UI_SET_KEYBIT, BTN_EXTRA);

  struct uinput_setup usetup;
  memset(&usetup, 0, sizeof(usetup));
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor = 0x9999;
  usetup.id.product = 0x8888;
  strcpy(usetup.name, "Mouse45 Fusion Driver");
  ioctl(virt_fd, UI_DEV_SETUP, &usetup);
  ioctl(virt_fd, UI_DEV_CREATE);

  pthread_t t;
  pthread_create(&t, NULL, output_thread, NULL);

  char error[256] = {0};
  bool reselect = !loaded;

  while (atomic_load(&keep_running)) {
    if (reselect || strlen(settings.mouse_path) == 0) {
      if (select_device(settings.mouse_path, sizeof(settings.mouse_path), error,
                        settings.mouse_path, "event-mouse", "Mouse") != 0) {
        if (access(settings.mouse_path, F_OK) != 0)
          cleanup(0);
      } else {
        save_settings();
      }
      error[0] = 0;
    }

    if (reselect || strlen(settings.kbd_path) == 0) {
      if (select_device(settings.kbd_path, sizeof(settings.kbd_path), error,
                        settings.kbd_path, "event-kbd", "Keyboard") != 0) {
        if (access(settings.kbd_path, F_OK) != 0)
          cleanup(0);
      } else {
        save_settings();
      }
      error[0] = 0;
    }

    reselect = false;

    draw_main_ui();

    mouse_fd = open(settings.mouse_path, O_RDONLY);
    if (mouse_fd < 0) {
      snprintf(error, sizeof(error), "Mouse Open Fail: %s", strerror(errno));
      reselect = true;
      sleep(1);
      continue;
    }

    wait_for_release(mouse_fd, "Mouse");

    if (ioctl(mouse_fd, EVIOCGRAB, 1) < 0) {
      snprintf(error, sizeof(error), "Mouse Grab Fail (sudo?)");
      close(mouse_fd);
      reselect = true;
      sleep(1);
      continue;
    }

    kbd_fd = open(settings.kbd_path, O_RDONLY);
    if (kbd_fd < 0) {
      snprintf(error, sizeof(error), "Keyboard Open Fail: %s", strerror(errno));
      close(mouse_fd);
      reselect = true;
      sleep(1);
      continue;
    }

    wait_for_release(kbd_fd, "Keyboard");

    if (ioctl(kbd_fd, EVIOCGRAB, 1) < 0) {
      snprintf(error, sizeof(error), "Keyboard Grab Fail (sudo?)");
      close(mouse_fd);
      close(kbd_fd);
      reselect = true;
      sleep(1);
      continue;
    }

    sync_device_state(mouse_fd, virt_fd);

    struct pollfd fds[3];
    fds[0].fd = mouse_fd;
    fds[0].events = POLLIN;
    fds[1].fd = kbd_fd;
    fds[1].events = POLLIN;
    fds[2].fd = STDIN_FILENO;
    fds[2].events = POLLIN;

    struct input_event ev;
    bool active = true;
    bool is_ctrl_held = false;

    while (active && atomic_load(&keep_running)) {
      if (poll(fds, 3, -1) > 0) {
        if (fds[0].revents & POLLIN) {
          if (read(mouse_fd, &ev, sizeof(ev)) > 0) {
            bool hooked = false;
            if (ev.type == EV_KEY && settings.scroll_enabled && !is_ctrl_held) {
              if (ev.code == BTN_EXTRA) {
                atomic_store(&action_scroll_up, ev.value != 0);
                hooked = true;
              } else if (ev.code == BTN_SIDE) {
                atomic_store(&action_scroll_down, ev.value != 0);
                hooked = true;
              }
            }
            if (!hooked) {
              pthread_mutex_lock(&uinput_lock);
              write(virt_fd, &ev, sizeof(ev));
              pthread_mutex_unlock(&uinput_lock);
            }
          } else {
            active = false;
            snprintf(error, sizeof(error), "Mouse Disconnected");
          }
        }

        if (fds[1].revents & POLLIN) {
          if (read(kbd_fd, &ev, sizeof(ev)) > 0) {
            bool hooked = false;

            if (ev.type == EV_KEY) {
              if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
                is_ctrl_held = (ev.value != 0);
              }
            }

            if (ev.type == EV_KEY && ev.code == KEY_GRAVE &&
                settings.bhop_enabled) {
              atomic_store(&action_bhop, ev.value != 0);
              hooked = true;
            }

            // Hook for the J key spammer
            if (ev.type == EV_KEY && ev.code == 36 /* KEY_J */ &&
                settings.f_spam_enabled) {
              atomic_store(&action_f_spam, ev.value != 0);
              hooked = true;
            }

            if (!hooked) {
              pthread_mutex_lock(&uinput_lock);
              write(virt_fd, &ev, sizeof(ev));
              pthread_mutex_unlock(&uinput_lock);
            }
          } else {
            active = false;
            snprintf(error, sizeof(error), "Keyboard Disconnected");
          }
        }

        if (fds[2].revents & POLLIN) {
          int c = getch();
          if (c == 'b') {
            settings.bhop_enabled = !settings.bhop_enabled;
            if (!settings.bhop_enabled)
              atomic_store(&action_bhop, false);
            save_settings();
            draw_main_ui();
          } else if (c == 's') {
            settings.scroll_enabled = !settings.scroll_enabled;
            if (!settings.scroll_enabled) {
              atomic_store(&action_scroll_up, false);
              atomic_store(&action_scroll_down, false);
            }
            save_settings();
            draw_main_ui();
          } else if (c == 'f' || c == 'F') { // UI toggle changed to 'f'
            settings.f_spam_enabled = !settings.f_spam_enabled;
            if (!settings.f_spam_enabled) {
              atomic_store(&action_f_spam, false);
            }
            save_settings();
            draw_main_ui();
          } else if (c == 'z') {
            active = false;
            reselect = true;
          } else if (c == 'q' || c == 3) {
            cleanup(0);
          }
        }
      }
    }

    release_all_keys(virt_fd);

    if (mouse_fd >= 0) {
      ioctl(mouse_fd, EVIOCGRAB, 0);
      close(mouse_fd);
      mouse_fd = -1;
    }
    if (kbd_fd >= 0) {
      ioctl(kbd_fd, EVIOCGRAB, 0);
      close(kbd_fd);
      kbd_fd = -1;
    }
  }

  pthread_join(t, NULL);
  cleanup(0);
  return 0;
}

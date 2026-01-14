#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
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
#include <termios.h>
#include <unistd.h>
#include <wordexp.h>

#define SCROLL_DELAY_US 35000 // 35ms
#define BHOP_DELAY_US 20000   // 20ms
#define MAX_DEVICES 50
#define MAX_PATH_LEN 256
#define CONFIG_DIR "~/.config/mouse45"
#define CONFIG_MOUSE CONFIG_DIR "/mouse.txt"
#define CONFIG_KBD CONFIG_DIR "/keyboard.txt"
#define CONFIG_STATUS CONFIG_DIR "/status.txt"

#define ANSI_RESET "\033[0m"
#define ANSI_GREEN "\033[32m"
#define ANSI_RED "\033[31m"
#define ANSI_CLEAR_SCREEN "\033[2J\033[H"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_HIDE_CURSOR "\033[?25l"

int mouse_fd = -1;
int kbd_fd = -1;
int virt_fd = -1;
atomic_bool keep_running = true;

pthread_mutex_t uinput_lock = PTHREAD_MUTEX_INITIALIZER;

bool toggle_scroll = true;
bool toggle_bhop = true;

atomic_bool action_scroll_up = false;
atomic_bool action_scroll_down = false;
atomic_bool action_bhop = false;

struct termios orig_termios;

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

void update_status_file() {
  ensure_config_dir_exists();
  char full_path[MAX_PATH_LEN];
  get_expanded_path(CONFIG_STATUS, full_path);
  FILE *f = fopen(full_path, "w");
  if (f) {
    fprintf(f, "Bhop: %s\nScroll: %s\n", toggle_bhop ? "ON" : "OFF",
            toggle_scroll ? "ON" : "OFF");
    fclose(f);
  }
}

void load_status() {
  char full_path[MAX_PATH_LEN];
  get_expanded_path(CONFIG_STATUS, full_path);
  FILE *f = fopen(full_path, "r");
  if (!f)
    return;

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "Bhop:")) {
      if (strstr(line, "OFF"))
        toggle_bhop = false;
      else if (strstr(line, "ON"))
        toggle_bhop = true;
    }
    if (strstr(line, "Scroll:")) {
      if (strstr(line, "OFF"))
        toggle_scroll = false;
      else if (strstr(line, "ON"))
        toggle_scroll = true;
    }
  }
  fclose(f);
}

void save_pref(const char *config_file, const char *device_path) {
  ensure_config_dir_exists();
  char full_path[MAX_PATH_LEN];
  get_expanded_path(config_file, full_path);
  FILE *f = fopen(full_path, "w");
  if (f) {
    fprintf(f, "%s", device_path);
    fclose(f);
  }
}

int load_pref(const char *config_file, char *device_path, size_t len) {
  char full_path[MAX_PATH_LEN];
  get_expanded_path(config_file, full_path);
  FILE *f = fopen(full_path, "r");
  if (f) {
    if (fgets(device_path, len, f) != NULL) {
      device_path[strcspn(device_path, "\n")] = 0;
      fclose(f);
      if (access(device_path, F_OK) == 0) {
        return 0;
      }
    } else {
      fclose(f);
    }
  }
  return -1;
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

void reset_terminal_mode() {
  printf(ANSI_SHOW_CURSOR);
  tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void set_raw_terminal_mode() {
  struct termios new_termios;
  tcgetattr(STDIN_FILENO, &orig_termios);
  memcpy(&new_termios, &orig_termios, sizeof(new_termios));
  new_termios.c_lflag &= ~(ICANON | ECHO);
  new_termios.c_cc[VMIN] = 1;
  new_termios.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
  printf(ANSI_HIDE_CURSOR);
}

void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  struct input_event ie;
  memset(&ie, 0, sizeof(ie));
  ie.type = type;
  ie.code = code;
  ie.value = value;
  write(fd, &ie, sizeof(ie));
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

void print_ui() {
  printf(ANSI_CLEAR_SCREEN);
  printf("\rmouse45 running\n");
  printf("---------------------------------------------------------\n");

  printf(" [s] Scrolling  : ");
  if (toggle_scroll)
    printf(ANSI_GREEN "ON " ANSI_RESET);
  else
    printf(ANSI_RED "OFF" ANSI_RESET);
  printf(" (Side buttons)\n");

  printf(" [b] Bhop       : ");
  if (toggle_bhop)
    printf(ANSI_GREEN "ON " ANSI_RESET);
  else
    printf(ANSI_RED "OFF" ANSI_RESET);
  printf(" (Hold `)\n");

  printf("---------------------------------------------------------\n");
  printf(" BackBtn        : Scroll Down | Ctrl + BackBtn : Back\n");
  printf(" FwdBtn         : Scroll Up   | Ctrl + FwdBtn  : Forward\n");
  printf("---------------------------------------------------------\n");
  printf(" [z] Reselect Devices | [q] Quit\n");
  fflush(stdout);
}

int select_device(char *selected_path, size_t len, const char *error_msg,
                  const char *current_pref, const char *filter,
                  const char *title) {
  char *dev_names[MAX_DEVICES];
  int count = 0;
  struct dirent *entry;
  const char *dev_dir = "/dev/input/by-id/";

  DIR *dp = opendir(dev_dir);
  if (!dp)
    return -1;

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
    fprintf(stderr, "No devices ending in '%s' found.\n", filter);
    sleep(2);
    return -1;
  }

  reset_terminal_mode();
  printf(ANSI_CLEAR_SCREEN);

  if (error_msg && *error_msg) {
    printf(ANSI_RED "Error: %s" ANSI_RESET "\n\n", error_msg);
  }

  printf("Select %s:\n", title);
  printf("----------------------------------\n");
  for (int i = 0; i < count; i++) {
    char *marker = "";
    if (current_pref && strstr(current_pref, dev_names[i])) {
      marker = " [Current]";
    }
    printf("%d) %s%s\n", i + 1, dev_names[i], marker);
  }
  printf("----------------------------------\n");

  int choice = -1;
  char buffer[64];

  while (choice == -1) {
    printf("Choice (1-%d) or 'q' to go back: ", count);
    if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
      // eof or err
      break;
    }

    buffer[strcspn(buffer, "\n")] = 0;

    if (strcmp(buffer, "q") == 0 || strcmp(buffer, "Q") == 0) {
      for (int i = 0; i < count; i++)
        free(dev_names[i]);
      return -1;
    }

    char *endptr;
    long val = strtol(buffer, &endptr, 10);

    if (endptr == buffer || val < 1 || val > count) {
      printf("Invalid selection. Please enter a number between 1 and %d.\n",
             count);
    } else {
      choice = (int)val;
    }
  }

  if (choice != -1) {
    snprintf(selected_path, len, "%s%s", dev_dir, dev_names[choice - 1]);
  }

  for (int i = 0; i < count; i++) {
    free(dev_names[i]);
  }

  return (choice != -1) ? 0 : -1;
}

void *output_thread(void *arg) {
  int sc_timer = 0;
  int bh_timer = 0;
  while (atomic_load(&keep_running)) {
    bool up = atomic_load(&action_scroll_up);
    bool down = atomic_load(&action_scroll_down);
    bool hop = atomic_load(&action_bhop);

    if (!up && !down && !hop) {
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

    pthread_mutex_unlock(&uinput_lock);
    usleep(5000);
  }
  return NULL;
}

void cleanup(int signo) {
  reset_terminal_mode();
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

  printf("\nExited cleanly.\n");
  exit(0);
}

int main(void) {
  tcgetattr(STDIN_FILENO, &orig_termios);

  printf(ANSI_CLEAR_SCREEN);
  signal(SIGINT, cleanup);
  signal(SIGTERM, cleanup);
  load_status();
  update_status_file();

  virt_fd = open("/dev/uinput", O_WRONLY);
  if (virt_fd < 0) {
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

  char m_path[MAX_PATH_LEN] = {0}, k_path[MAX_PATH_LEN] = {0};
  char error[256] = {0};
  bool reselect = false;

  while (atomic_load(&keep_running)) {
    if (reselect || load_pref(CONFIG_MOUSE, m_path, sizeof(m_path)) != 0) {
      if (select_device(m_path, sizeof(m_path), error, m_path, "event-mouse",
                        "Mouse") != 0) {
        if (access(m_path, F_OK) != 0)
          cleanup(0);
        reselect = false;
      } else {
        save_pref(CONFIG_MOUSE, m_path);
      }
      error[0] = 0;
    }

    if (reselect || load_pref(CONFIG_KBD, k_path, sizeof(k_path)) != 0) {
      if (select_device(k_path, sizeof(k_path), error, k_path, "event-kbd",
                        "Keyboard") != 0) {
        if (access(k_path, F_OK) != 0)
          cleanup(0);
        reselect = false;
      } else {
        save_pref(CONFIG_KBD, k_path);
      }
      error[0] = 0;
    }

    reselect = false;

    mouse_fd = open(m_path, O_RDONLY);
    if (mouse_fd < 0) {
      snprintf(error, sizeof(error), "Mouse Open Fail: %s", strerror(errno));
      reselect = true;
      sleep(1);
      continue;
    }
    if (ioctl(mouse_fd, EVIOCGRAB, 1) < 0) {
      snprintf(error, sizeof(error), "Mouse Grab Fail (sudo?)");
      close(mouse_fd);
      reselect = true;
      sleep(1);
      continue;
    }

    kbd_fd = open(k_path, O_RDONLY);
    if (kbd_fd < 0) {
      snprintf(error, sizeof(error), "Keyboard Open Fail: %s", strerror(errno));
      close(mouse_fd);
      reselect = true;
      sleep(1);
      continue;
    }

    // Brief pause to ensure user has released Enter from selection menu
    // before we grab the keyboard, preventing stuck key states
    sleep(1);

    if (ioctl(kbd_fd, EVIOCGRAB, 1) < 0) {
      snprintf(error, sizeof(error), "Keyboard Grab Fail (sudo?)");
      close(mouse_fd);
      close(kbd_fd);
      reselect = true;
      sleep(1);
      continue;
    }
    set_raw_terminal_mode();
    struct pollfd fds[3];
    fds[0].fd = mouse_fd;
    fds[0].events = POLLIN;
    fds[1].fd = kbd_fd;
    fds[1].events = POLLIN;
    fds[2].fd = STDIN_FILENO;
    fds[2].events = POLLIN;

    print_ui();

    struct input_event ev;
    bool active = true;
    bool is_ctrl_held = false;

    while (active && atomic_load(&keep_running)) {
      if (poll(fds, 3, -1) > 0) {
        if (fds[0].revents & POLLIN) {
          if (read(mouse_fd, &ev, sizeof(ev)) > 0) {
            bool hooked = false;
            if (ev.type == EV_KEY && toggle_scroll && !is_ctrl_held) {
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

            if (ev.type == EV_KEY && ev.code == KEY_GRAVE && toggle_bhop) {
              atomic_store(&action_bhop, ev.value != 0);
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
          char c;
          if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'b') {
              toggle_bhop = !toggle_bhop;
              if (!toggle_bhop)
                atomic_store(&action_bhop, false);
              update_status_file();
              print_ui();
            } else if (c == 's') {
              toggle_scroll = !toggle_scroll;
              if (!toggle_scroll) {
                atomic_store(&action_scroll_up, false);
                atomic_store(&action_scroll_down, false);
              }
              update_status_file();
              print_ui();
            } else if (c == 'z') {
              active = false;
              reselect = true;
            } else if (c == 'q' || c == 3) {
              cleanup(0);
            }
          }
        }
      }
    }

    release_all_keys(virt_fd);

    reset_terminal_mode();
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

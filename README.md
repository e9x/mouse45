# mouse45

A scrolling utility for Linux/Wayland/X11 that simulates scrolling and has bhopping.

## Prerequisites

Before building, make sure you have all the required libaries for building C code (gcc)

Debian/Ubuntu:

```sh
sudo apt update
sudo apt install build-essential
```

Fedora:

```sh
sudo dnf groupinstall "Development Tools"
sudo dnf install kernel-headers
```

Arch:

```sh
sudo pacman -Syu base-devel
```

## Building

```sh
make clean
make
```

## Install

If you want to use the .desktop file, you will need xfce4-terminal

Debian/Ubuntu:

```sh
sudo apt install xfce4-terminal
```

Fedora:

```sh
sudo dnf install xfce4-terminal
```

Arch:

```sh
sudo pacman -S xfce4-terminal
```

## Building

```sh
git clone https://github.com/e9x/mouse45
cd mouse45
make
```

Optionally install the shortcuts:

```sh
make install
```

## Permissions

You may find that running mouse45 gives errors relating to sudo and not being able to access a device with uinput. You can configure uinput roles like this:

```sh
echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-uinput.rules
```

Make yourself a user too

```sh
sudo usermod -aG input $USER
```

If the above is too much then

## Running

Start the program (requires root):

```sh
./bin/mouse45
```

## Controls & Keybinds

Once the program is running and the devices are selected, the following keybinds are active:

### Mouse Features

| Trigger                             | Action             | Description                             |
| :---------------------------------- | :----------------- | :-------------------------------------- |
| **Mouse Forward Button**            | **Scroll Up**      | Simulates mouse wheel scrolling up.     |
| **Mouse Back Button**               | **Scroll Down**    | Simulates mouse wheel scrolling down.   |
| <kbd>Ctrl</kbd> + Mouse Forward\*\* | **Native Forward** | Stops simulating mouse wheel while held |
| **Ctrl/kbd> + Mouse Back**          | **Native Back**    | Sends the back button event normally    |

### Keyboard Features

| Trigger                         | Action        | Description                                     |
| :------------------------------ | :------------ | :---------------------------------------------- |
| Hold <kbd>`</kbd> (grave/tilde) | **Bunny Hop** | Spams the **Spacebar** while held (for gaming). |

### Terminal Management

When the terminal window running `mouse45` is in focus:

| Key          | Function             | Description                                                |
| :----------- | :------------------- | :--------------------------------------------------------- |
| <kbd>s</kbd> | **Toggle Scrolling** | Enables/Disables the side-button scrolling feature.        |
| <kbd>b</kbd> | **Toggle Bhop**      | Enables/Disables the bunny hop macro <kbd>`</kbd> feature. |
| <kbd>z</kbd> | **Reselect Devices** | Prompts you to pick devices again.                         |
| <kbd>q</kbd> | **Quit**             | Exit                                                       |

# Help

Feel free to open an issue: https://github.com/e9x/mouse45/issues

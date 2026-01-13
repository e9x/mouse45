# mouse45

A scrolling utility for Linux/X11 that simulates scrolling and has bhopping.

## Prerequisites

Before building, install X11 libraries:

Debian/Ubuntu:

```sh
sudo apt install build-essential libx11-dev libxtst-dev
```

Fedora:

```sh
sudo dnf install gcc make libX11-devel libXtst-devel
```

Arch:

```sh
sudo pacman -S base-devel libx11 libxtst
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

## Setup & Configuration

### 1. X11 Configuration

You must enable mouse wheel emulation in X11 for this to work.

Create `/etc/X11/xorg.conf.d/99-mouse-emulation.conf` and add:

```
Section "InputClass"
    Identifier "evdev pointer catchall"
    MatchIsPointer "on"
    MatchDevicePath "/dev/input/event*"
    Driver "evdev"
    Option "EmulateWheel"	"true"
    Option "EmulateWheelButton"    "2"
    Option "XAxisMapping"	"6 7"
    Option "YAxisMapping"	"4 5"
EndSection
```

### 2. Button Remapping

Install [input-remapper](https://github.com/sezanzeb/input-remapper):

Debian/Ubuntu:

```sh
wget https://github.com/sezanzeb/input-remapper/releases/download/2.2.0/input-remapper-2.2.0.deb
sudo apt install -f ./input-remapper-2.2.0.deb
```

Or:

```sh
sudo apt install input-remapper
```

Fedora:

```sh
sudo dnf install input-remapper
sudo systemctl enable --now input-remapper
```

Arch:

```sh
yay -S input-remapper-git
sudo systemctl enable --now input-remapper
```

### Configure

1. Run `input-remapper-gtk`
2. Select your mouse
3. Map your lower side button to <kbd>Alt</kbd>+<kbd>]</kbd>: `ALT_L + bracketleft`
4. Map your upper side button to <kbd>Alt</kbd>+<kbd>]</kbd>: `ALT_L + bracketright`

When the script is ran, the macro for the bracket keys will be triggered by the side buttons and will scroll. 

## Running

Start the program:

```sh
./mouse45
```

## Controls / Usage

- <kbd>Alt</kbd>+<kbd>[</kbd> : Scroll Down | <kbd>Ctrl</kbd>+<kbd>[</kbd> : Back
- <kbd>Alt</kbd>+<kbd>]</kbd> : Scroll Up | <kbd>Ctrl</kbd>+<kbd>]</kbd> : Forward
- <kbd>`</kbd> : Bhop Hold (Toggle with <kbd>b</kbd>)
- <kbd>s</kbd> : Toggle Mouse/Brackets On/Off

_Focus the window and press <kbd>q</kbd> to quit._

## Example Remapper Config

My `input-remapper` JSON config for a Logitech G203 (from `~/.config/input-remapper-2/presets/Logitech\ G203\ LIGHTSYNC\ Gaming\ Mouse/scroll\ it.json`)

```json
[
    {
        "input_combination": [
            {
                "type": 1,
                "code": 276,
                "origin_hash": "32844196a41f9bfae3ab69ea16314300"
            }
        ],
        "target_uinput": "keyboard",
        "output_symbol": "ALT_L + bracketright",
        "mapping_type": "key_macro"
    },
    {
        "input_combination": [
            {
                "type": 1,
                "code": 275,
                "origin_hash": "32844196a41f9bfae3ab69ea16314300"
            }
        ],
        "target_uinput": "keyboard",
        "output_symbol": "\tALT_L + bracketleft",
        "mapping_type": "key_macro"
    }
]

```

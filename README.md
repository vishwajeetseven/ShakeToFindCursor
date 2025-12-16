# Shake Cursor for Windows

A lightweight, high-performance Windows utility that replicates the popular macOS **"Shake to Find"** cursor feature.

Written in pure C++ (Win32 API & GDI+), this application draws a smooth, high-visibility overlay around your mouse pointer when you shake it, helping you locate your cursor instantly on large or multi-monitor setups.

![Project Status](https://img.shields.io/badge/status-active-brightgreen)
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B-00599C)

## Features

* **Zero Lag:** Uses `UpdateLayeredWindow` for hardware-accelerated, game-like performance (100+ FPS).
* **macOS Style:** Smooth animation with physics-based growth and decay.
* **Customization:**
    * Default: High-visibility **Red Ring**.
    * Custom: Support for loading your own **PNG** or **ICO** images (e.g., giant arrows, crosshairs, memes).
* **Unobtrusive:**
    * **Click-Through:** The overlay is transparent to mouse clicks; it never interferes with your work.
    * **System Tray:** Runs silently in the background with a tray icon for options.
* **Lightweight:** Consumes negligible CPU/RAM when not shaking.

## Installation & Usage

### Option 1: Build from Source
This project is designed to be compiled with **Dev-C++** (MinGW/GCC), but works with any standard C++ compiler that supports Win32 API.

1.  Clone this repository.
2.  Open `main.cpp` in Dev-C++.
3.  **Crucial Step:** Go to `Project` -> `Project Options` -> `Parameters` -> `Linker` and add:
    ```text
    -lgdiplus
    ```
4.  Compile and Run (F11).

### Usage
1.  Run the application. It will start silently in the background.
2.  **Shake your mouse** vigorously to see the cursor highlight.
3.  **Right-click** the small arrow icon in your System Tray (bottom right) to:
    * Select a custom image (PNG/ICO).
    * Reset to the default Red Circle.
    * Exit the program.

## Technical Details

* **Language:** C++
* **Libraries:** `windows.h`, `gdiplus.h`
* **Rendering:** Uses Layered Windows with GDI+ for anti-aliased, alpha-blended rendering that sits on top of all other windows.
* **Physics:** Implements a custom "Shake Score" algorithm that detects direction reversals rather than just speed, preventing false triggers during normal fast movements.

## Configuration (Code Level)

You can tweak the constants at the top of `main.cpp` to adjust the feel:

```cpp
const int WINDOW_SIZE = 300;       // Max render area size
const int SHAKE_THRESHOLD = 15;    // Sensitivity (Lower = easier to trigger)
const int SHAKE_DECAY = 5;         // How fast it shrinks
const int SHAKE_GROWTH = 35;       // How fast it grows
const float SMOOTH_FACTOR = 0.35f; // Animation speed (Lerp)

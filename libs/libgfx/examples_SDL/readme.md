# GFX Library Examples

Three simple examples to get started with the GFX library and SDL backend.

## Build Instructions

### Prerequisites
```bash
# Install SDL2
# Ubuntu/Debian
sudo apt-get install libsdl2-dev

# macOS
brew install sdl2

# Fedora
sudo dnf install SDL2-devel
```

### Build All Examples
```bash
mkdir build
cd build
cmake .. -DGFX_SDL_SUPPORT=ON
make
```

Or use the examples-specific CMakeLists.txt:
```bash
mkdir build
cd build
cmake -f ../CMakeLists_examples.txt ..
make
```

## Examples

### 1. example_simple
**File:** `example_simple.cpp`

The simplest example - just draws some shapes on screen.

**Features:**
- Creates a 800×600 window
- Draws a red circle, yellow rectangle, and green filled rectangle
- Runs until window is closed

**Run:**
```bash
./example_simple
```

---

### 2. example_input
**File:** `example_input.cpp`

Demonstrates mouse input handling with event callbacks.

**Features:**
- Tracks mouse movement
- Cyan circle follows cursor
- Prints click messages to console
- Shows on-screen instructions

**Run:**
```bash
./example_input
```

**Interaction:**
- Move mouse around the window
- Click to see console messages

---

### 3. example_paint
**File:** `example_paint.cpp`

A simple paint program using mouse drawing.

**Features:**
- White canvas background
- Draw by clicking and dragging
- Lines drawn from last position to current
- Console feedback on draw start/stop

**Run:**
```bash
./example_paint
```

**Interaction:**
- Click and drag to draw
- Each line segment connects to previous position

---

## Code Structure

Each example follows this basic pattern:

```cpp
#include "GFX.h"

int main() {
    // 1. Create window
    LinuxGFX gfx("Title", 800, 600);
    
    // 2. Optional: Set event callback
    gfx.setEventCallback([](const GFXInputEvent& event) {
        // Handle events
    });
    
    // 3. Main loop
    while (gfx.processEvents()) {
        // Clear and draw
        gfx.fillScreen(GFX_BLACK);
        // ... draw stuff ...
        gfx.swapBuffers();
    }
    
    return 0;
}
```

## Common GFX Functions Used

### Window & Screen
- `LinuxGFX(title, width, height)` - Create window
- `processEvents()` - Handle events and input
- `fillScreen(color)` - Clear screen
- `swapBuffers()` - Update display

### Drawing
- `fillCircle(x, y, radius, color)` - Filled circle
- `drawRect(x, y, w, h, color)` - Rectangle outline
- `fillRect(x, y, w, h, color)` - Filled rectangle
- `drawLine(x0, y0, x1, y1, color)` - Line

### Text
- `setTextColor(color)` - Set text color
- `setCursor(x, y)` - Set text position
- `writeText(string)` - Draw text

### Events & Input
- `setEventCallback(callback)` - Register event handler
- `getMousePosition(x, y)` - Get current mouse position

### Colors (Predefined)
- `GFX_BLACK`, `GFX_WHITE`, `GFX_RED`, `GFX_GREEN`, `GFX_BLUE`
- `GFX_YELLOW`, `GFX_CYAN`, `GFX_MAGENTA`

## Customization

Easy ways to modify the examples:

### Change window size
```cpp
LinuxGFX gfx("Title", 1024, 768);  // Change dimensions
```

### Change colors
```cpp
gfx.fillScreen(GFX_BLUE);           // Use different color
gfx.fillCircle(x, y, r, GFX_RED);   // or create custom
```

### Create custom colors
```cpp
uint32_t purple = gfx.colorARGB(255, 128, 0, 128);
gfx.fillRect(x, y, w, h, purple);
```

### Add more drawing
```cpp
gfx.drawCircle(400, 300, 100, GFX_YELLOW);  // Outline
gfx.drawTriangle(100, 100, 200, 50, 300, 100, GFX_WHITE);
gfx.drawRoundRect(50, 50, 200, 150, 20, GFX_GREEN);
```

## Troubleshooting

### "SDL2 not found"
Install SDL2 development package (see Prerequisites above)

### Window won't display
- Make sure SDL2 is properly linked
- Check that -DGFX_SDL_SUPPORT=ON is set in CMake
- Verify libgfx compiled with GFXSDL defined

### Program crashes
- Check that coordinates are within window bounds
- Ensure window is created successfully (check console output)

## Learning Path

1. **Start with `example_simple`** - Learn basic drawing
2. **Move to `example_input`** - Understand event handling
3. **Try `example_paint`** - Combine drawing + input

## API Reference

Full API documentation is available in:
- `API_REFERENCE.md` - Complete API documentation
- `SDL_EXAMPLE.md` - Detailed SDL usage guide

## Next Steps

- Modify examples to draw different shapes
- Add text rendering with `setFont()` and `writeText()`
- Experiment with different colors and sizes
- Build your own graphics application!

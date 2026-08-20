#include "GFX.h"
#include <iostream>

int main() {
    // Create a 800x600 window
    LinuxGFX gfx("Simple GFX Demo", 800, 600);
    
    // Main loop
    while (gfx.processEvents()) {
        // Clear screen
        gfx.fillScreen(GFX_BLACK);
        
        // Draw some shapes
        gfx.fillCircle(400, 300, 50, GFX_RED);
        gfx.drawRect(100, 100, 200, 150, GFX_YELLOW);
        gfx.fillRect(550, 450, 100, 80, GFX_GREEN);
        
        // Update display
        gfx.swapBuffers();
    }
    
    return 0;
}

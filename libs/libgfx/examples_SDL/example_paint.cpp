#include "GFX.h"
#include <iostream>

int main() {
    LinuxGFX gfx("Paint Demo", 1024, 768);
    
    int16_t lastX = -1, lastY = -1;
    bool isDrawing = false;
    
    // Clear screen to white
    gfx.fillScreen(GFX_WHITE);
    gfx.swapBuffers();
    
    // Event callback for drawing
    gfx.setEventCallback([&](const GFXInputEvent& event) {
        if (event.type == GFXEventType::MOUSE_BUTTON_DOWN) {
            isDrawing = true;
            lastX = event.x;
            lastY = event.y;
            std::cout << "Drawing started\n";
        }
        else if (event.type == GFXEventType::MOUSE_BUTTON_UP) {
            isDrawing = false;
            std::cout << "Drawing stopped\n";
        }
        else if (event.type == GFXEventType::MOUSE_MOVE && isDrawing) {
            // Draw line from last position to current
            if (lastX >= 0 && lastY >= 0) {
                gfx.drawLine(lastX, lastY, event.x, event.y, GFX_BLACK);
            }
            lastX = event.x;
            lastY = event.y;
        }
    });
    
    // Main loop
    while (gfx.processEvents()) {
        gfx.swapBuffers();
    }
    
    return 0;
}

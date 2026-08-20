#pragma once
#include <imgui.h>
#include <Simtary.h>
#include <wiBacklog.h>

class BacklogViewer {
private:
    bool autoScroll = true;
    ImGuiTextFilter filter;

public:
    void render(bool *isopen);
};
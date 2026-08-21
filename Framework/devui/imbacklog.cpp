#include "imbacklog.h"
#include <sstream>

void BacklogViewer::render(bool *isopen) {
    ImGui::Begin("Backlog Viewer", isopen);

    // --- Menu Bar ---
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Options")) {
            ImGui::MenuItem("Auto-scroll", nullptr, &autoScroll);
            if (ImGui::MenuItem("Clear")) {
                wi::backlog::clear();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // --- Filter ---
    filter.Draw("Filter", -80.0f);
    ImGui::SameLine();
    if (ImGui::Button("Clear##filter")) filter.Clear();
    ImGui::Separator();

    // --- Log Content ---
    ImGui::BeginChild("ScrollRegion", ImVec2(0, 0), 
        false, ImGuiWindowFlags_HorizontalScrollbar);

    std::string fullLog = wi::backlog::getText();
    std::istringstream stream(fullLog);
    std::string line;

    while (std::getline(stream, line)) {
        if (!filter.PassFilter(line.c_str())) continue;

        // Color coding based on log level
        if (line.find("[ERROR]") != std::string::npos ||
            line.find("INVALID") != std::string::npos ||
            line.find("missing") != std::string::npos) {
            ImGui::PushStyleColor(ImGuiCol_Text, 
                ImVec4(1.0f, 0.3f, 0.3f, 1.0f)); // red
        } else if (line.find("[WARNING]") != std::string::npos) {
            ImGui::PushStyleColor(ImGuiCol_Text, 
                ImVec4(1.0f, 0.8f, 0.0f, 1.0f)); // yellow
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, 
                ImVec4(0.8f, 0.8f, 0.8f, 1.0f)); // default gray
        }

        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }

    // Auto-scroll to bottom
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}
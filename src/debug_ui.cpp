/*
 * debug_ui.cpp - ImGui debug overlay implementation
 */
#include "debug_ui.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include <GL/glew.h>
#include <stdio.h>

extern "C" {

DebugState g_debug = {0};

void debug_ui_init(SDL_Window *window, SDL_GLContext gl_ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = NULL; /* Don't save layout */

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.Alpha = 0.9f;
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void debug_ui_shutdown(void) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void debug_ui_process_event(SDL_Event *event) {
    ImGui_ImplSDL2_ProcessEvent(event);

    /* INSERT key toggles debug UI */
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_INSERT) {
        g_debug.visible = !g_debug.visible;
        /* Release mouse when debug UI opens */
        if (g_debug.visible)
            SDL_SetRelativeMouseMode(SDL_FALSE);
    }
}

bool debug_ui_wants_mouse(void) {
    if (!g_debug.visible) return false;
    return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

void debug_ui_render(void) {
    if (!g_debug.visible) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    /* ---- Main debug window ---- */
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug Menu (INSERT to toggle)")) {

        /* Performance */
        ImGui::Text("FPS: %.1f (%.2f ms)", g_debug.fps, g_debug.dt * 1000.0f);
        ImGui::Text("Draw calls: %d", g_debug.draw_calls);
        ImGui::Separator();

        /* Player info */
        if (ImGui::CollapsingHeader("Player Info", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Position: (%.1f, %.1f, %.1f)",
                        g_debug.player_x, g_debug.player_y, g_debug.player_z);
            ImGui::Text("Yaw: %.1f  Pitch: %.1f",
                        g_debug.player_yaw * 57.2958f,
                        g_debug.player_pitch * 57.2958f);
            ImGui::Text("Sector: %d", g_debug.player_sector);
            ImGui::Text("Health: %d", g_debug.player_health);
        }

        /* Level / mission info */
        if (ImGui::CollapsingHeader("Level / Mission", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Map: %s", g_debug.map_name[0] ? g_debug.map_name : "(none)");
            if (g_debug.campaign_active)
                ImGui::Text("Story: mission %d / %d",
                            g_debug.campaign_mission, g_debug.campaign_total);
            else
                ImGui::TextDisabled("Single map (not campaign)");
            ImGui::Separator();
            if (g_debug.has_boss) {
                ImGui::Text("Objective: %s",
                            g_debug.objective[0] ? g_debug.objective : "(none)");
                ImVec4 bc = g_debug.boss_dead  ? ImVec4(0.4f,1,0.4f,1)
                          : g_debug.boss_spawned ? ImVec4(1,0.5f,0.3f,1)
                          : ImVec4(0.7f,0.7f,0.7f,1);
                ImGui::TextColored(bc, "Boss: %s",
                    g_debug.boss_dead ? "DEFEATED"
                    : g_debug.boss_spawned ? "ACTIVE" : "dormant");
            } else {
                ImGui::TextDisabled("No boss objective");
            }
            ImGui::TextColored(
                g_debug.mission_complete ? ImVec4(0.4f,1,0.4f,1) : ImVec4(1,1,1,1),
                "Status: %s", g_debug.mission_complete ? "COMPLETE" : "in progress");
            ImGui::Separator();
            ImGui::Text("Enemies: %d / %d alive",
                        g_debug.enemies_alive, g_debug.enemies_total);
            ImGui::Text("Items left: %d", g_debug.items_alive);
            ImGui::Text("Sectors: %d   Entities: %d",
                        g_debug.sector_count, g_debug.entity_count);
        }

        ImGui::Separator();

        /* Render options */
        if (ImGui::CollapsingHeader("Render Options", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Wireframe", &g_debug.wireframe);
        }

        ImGui::Separator();

        /* Cheats */
        if (ImGui::CollapsingHeader("Cheats", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("God Mode", &g_debug.godmode);
            ImGui::Checkbox("Noclip", &g_debug.noclip);
            if (ImGui::Button("Give All Weapons + Ammo"))
                g_debug.give_all_weapons = true;
            if (ImGui::Button("Full Health"))
                g_debug.player_health = 100; /* Game loop reads this */
        }

        ImGui::Separator();

        /* Inventory — grant keys / tools (bit = 1 << InfKeyType).
         * STEEL=1 IRON=2 BRASS=3 ROUND=4 SQUARE=5 CROWBAR=6 GENERIC=7
         * SHOVEL=8 BADGE=9. Game loop ORs give_items into player.keys. */
        if (ImGui::CollapsingHeader("Inventory (grant items)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            struct { const char *label; int bit; } items[] = {
                { "Steel Key",       1 << 1 },
                { "Iron Key",        1 << 2 },
                { "Brass Key",       1 << 3 },
                { "Round Stone Key", 1 << 4 },
                { "Square Stone Key",1 << 5 },
                { "Crowbar",         1 << 6 },
                { "Shovel",          1 << 8 },
                { "Sheriff's Badge", 1 << 9 },
            };
            for (int i = 0; i < 8; i++) {
                if (ImGui::Button(items[i].label))
                    g_debug.give_items |= items[i].bit;
                if ((i & 1) == 0) ImGui::SameLine();
            }
            ImGui::NewLine();
            if (ImGui::Button("Give ALL Keys + Tools")) {
                for (int i = 0; i < 8; i++) g_debug.give_items |= items[i].bit;
            }
            /* Held items (read-back). */
            ImGui::Text("Held:");
            bool any = false;
            for (int i = 0; i < 8; i++) {
                if (g_debug.player_keys & items[i].bit) {
                    ImGui::SameLine(); ImGui::TextColored(
                        ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", items[i].label);
                    any = true;
                }
            }
            if (!any) { ImGui::SameLine(); ImGui::TextDisabled("(none)"); }
        }
    }
    ImGui::End();

    /* Render ImGui */
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} /* extern "C" */

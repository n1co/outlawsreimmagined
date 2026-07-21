/*
 * debug_ui.cpp - ImGui debug overlay (multi-window, FPS graph, level inspector)
 *
 * Robustness note: the game uses SDL RELATIVE mouse mode for FPS look. ImGui
 * needs ABSOLUTE mouse. While the overlay is visible we force absolute mode
 * every frame (right at NewFrame) so clicks/scroll always land — the previous
 * version intermittently lost clicks / lagged scroll because relative mode
 * left ImGui with a stale/garbage cursor position.
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
    style.Alpha = 0.95f;
    style.WindowRounding = 5.0f;
    style.FrameRounding  = 3.0f;
    style.GrabRounding   = 3.0f;
    style.WindowBorderSize = 1.0f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void debug_ui_shutdown(void) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void debug_ui_process_event(SDL_Event *event) {
    /* Only feed events to ImGui while the overlay is visible. The SDL2 backend
     * QUEUES every event (esp. mouse-motion) into ImGuiIO, but that queue is only
     * drained by ImGui::NewFrame — which we call solely when visible. If we fed
     * events while hidden, minutes of gameplay mouse-look would pile up thousands
     * of queued moves; on open, ImGui replays them one position-change per frame
     * (to never merge a move with a click), so the panels show a long stream of
     * hover animations and stay unresponsive until the backlog drains. Gating the
     * feed keeps the queue empty while hidden, so the UI is instant on open. */
    if (g_debug.visible)
        ImGui_ImplSDL2_ProcessEvent(event);

    /* INSERT (and F1) toggles debug UI */
    if (event->type == SDL_KEYDOWN &&
        (event->key.keysym.scancode == SDL_SCANCODE_INSERT ||
         event->key.keysym.scancode == SDL_SCANCODE_F1)) {
        g_debug.visible = !g_debug.visible;
        if (g_debug.visible)
            SDL_SetRelativeMouseMode(SDL_FALSE);  /* free cursor for the panels */
    }
}

bool debug_ui_wants_mouse(void) {
    if (!g_debug.visible) return false;
    return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

void debug_ui_push_fps(float fps) {
    int cap = (int)(sizeof(g_debug.fps_hist) / sizeof(g_debug.fps_hist[0]));
    g_debug.fps_hist[g_debug.fps_hist_head] = fps;
    g_debug.fps_hist_head = (g_debug.fps_hist_head + 1) % cap;
    if (g_debug.fps_hist_count < cap) g_debug.fps_hist_count++;
}

/* Ring buffer → linear array for ImGui::PlotLines (oldest→newest). */
static float s_fps_plot[128];
static int fps_plot_fill(void) {
    int cap = (int)(sizeof(g_debug.fps_hist) / sizeof(g_debug.fps_hist[0]));
    int n = g_debug.fps_hist_count;
    for (int i = 0; i < n; i++) {
        int idx = (g_debug.fps_hist_head - n + i + 2 * cap) % cap;
        s_fps_plot[i] = g_debug.fps_hist[idx];
    }
    return n;
}

void debug_ui_render(void) {
    if (!g_debug.visible) return;

    /* Force absolute mouse each frame while the overlay is up (see note). */
    if (SDL_GetRelativeMouseMode() == SDL_TRUE)
        SDL_SetRelativeMouseMode(SDL_FALSE);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiWindowFlags WF = ImGuiWindowFlags_NoCollapse;

    /* ---- Performance (top-left) ---- */
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 170), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Performance", nullptr, WF)) {
        ImVec4 fc = g_debug.fps >= 55 ? ImVec4(0.4f,1,0.4f,1)
                  : g_debug.fps >= 30 ? ImVec4(1,0.9f,0.3f,1)
                                      : ImVec4(1,0.4f,0.4f,1);
        ImGui::TextColored(fc, "%.0f FPS", g_debug.fps);
        ImGui::SameLine();
        ImGui::Text("(%.2f ms)", g_debug.dt * 1000.0f);
        int n = fps_plot_fill();
        if (n > 1) {
            char ov[32]; snprintf(ov, sizeof(ov), "%.0f fps", g_debug.fps);
            ImGui::PlotLines("##fps", s_fps_plot, n, 0, ov,
                             0.0f, 120.0f, ImVec2(-1, 70));
        }
        ImGui::Text("Draw calls: %d", g_debug.draw_calls);
    }
    ImGui::End();

    /* ---- Player (left, under perf) ---- */
    ImGui::SetNextWindowPos(ImVec2(10, 190), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 210), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Player", nullptr, WF)) {
        ImGui::Text("Pos  (%.1f, %.1f, %.1f)",
                    g_debug.player_x, g_debug.player_y, g_debug.player_z);
        ImGui::Text("Yaw %.1f°  Pitch %.1f°",
                    g_debug.player_yaw * 57.2958f, g_debug.player_pitch * 57.2958f);
        float spd = /* horizontal speed */
            (float)__builtin_sqrtf(g_debug.player_vx*g_debug.player_vx +
                                   g_debug.player_vz*g_debug.player_vz);
        ImGui::Text("Vel  (%.1f, %.1f, %.1f)  |xz|=%.1f",
                    g_debug.player_vx, g_debug.player_vy, g_debug.player_vz, spd);
        ImGui::Text("Sector %d  floor %.1f  ceil %.1f",
                    g_debug.player_sector, g_debug.sector_floor, g_debug.sector_ceil);
        ImGui::Text("Eye %.2f   %s   %s",
                    g_debug.eye_height,
                    g_debug.on_ground ? "grounded" : "AIRBORNE",
                    g_debug.crouching ? "CROUCH" : "stand");
        ImGui::Text("Floor tex: %s   %s",
                    g_debug.sector_floor_tex[0] ? g_debug.sector_floor_tex : "-",
                    g_debug.in_water ? "[WATER/SWIM]" : "");
        ImGui::Separator();
        int hp = g_debug.player_health;
        ImVec4 hc = hp > 50 ? ImVec4(0.4f,1,0.4f,1)
                  : hp > 20 ? ImVec4(1,0.9f,0.3f,1) : ImVec4(1,0.4f,0.4f,1);
        ImGui::TextColored(hc, "Health %d", hp);
        static const char *wnames[9] = {"Fist","Pistol","Rifle","Shotgun",
            "Dbl.Sgun","Sawed","Dynamite","Knife","Gatling"};
        int wi = g_debug.weapon_idx;
        ImGui::Text("Weapon: %s  (%d/%d)",
                    (wi>=0 && wi<9) ? wnames[wi] : "?",
                    g_debug.weapon_clip, g_debug.weapon_reserve);
    }
    ImGui::End();

    /* ---- Looking At (crosshair inspector) ---- */
    ImGui::SetNextWindowPos(ImVec2(10, 405), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 90), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Looking At", nullptr, WF)) {
        ImVec4 lc = g_debug.look_is_door ? ImVec4(0.5f,0.8f,1,1)
                  : g_debug.look_enemy >= 0 ? ImVec4(1,0.6f,0.5f,1)
                                            : ImVec4(0.85f,0.85f,0.85f,1);
        ImGui::TextColored(lc, "%s", g_debug.look_desc[0] ? g_debug.look_desc : "(nothing)");
        if (g_debug.look_dist > 0)
            ImGui::Text("dist %.1f   sector %d", g_debug.look_dist, g_debug.look_sector);
    }
    ImGui::End();

    /* ---- Level / Mission (top-right) ---- */
    ImGui::SetNextWindowPos(ImVec2(320, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Level / Mission", nullptr, WF)) {
        ImGui::Text("Map: %s", g_debug.map_name[0] ? g_debug.map_name : "(none)");
        static const char *dnames[5] = {"?","Easy","Medium","Hard","Hardest"};
        int d = (g_debug.difficulty>=1 && g_debug.difficulty<=4) ? g_debug.difficulty : 0;
        ImGui::Text("Difficulty: %s", dnames[d]);
        if (g_debug.campaign_active)
            ImGui::Text("Story: mission %d / %d",
                        g_debug.campaign_mission, g_debug.campaign_total);
        else
            ImGui::TextDisabled("Single map (not campaign)");
        ImGui::Separator();
        if (g_debug.has_boss) {
            ImGui::TextWrapped("Objective: %s",
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
        ImGui::Text("Enemies: %d / %d alive", g_debug.enemies_alive, g_debug.enemies_total);
        ImGui::Text("Items left: %d", g_debug.items_alive);
        ImGui::Text("Sectors: %d   Entities: %d",
                    g_debug.sector_count, g_debug.entity_count);
        ImGui::Separator();
        if (ImGui::Button("Reload Level")) g_debug.req_reload_level = 1;
        ImGui::SameLine();
        ImGui::TextDisabled("(set difficulty below reloads)");
        for (int i = 1; i <= 4; i++) {
            if (i > 1) ImGui::SameLine();
            char b[16]; snprintf(b, sizeof(b), "%s", dnames[i]);
            if (ImGui::Button(b)) g_debug.req_set_difficulty = i;
        }
    }
    ImGui::End();

    /* ---- Render / Cheats (right, under level) ---- */
    ImGui::SetNextWindowPos(ImVec2(320, 280), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Render / Cheats", nullptr, WF)) {
        ImGui::Checkbox("Wireframe", &g_debug.wireframe);
        ImGui::Separator();
        ImGui::Checkbox("God Mode", &g_debug.godmode);
        ImGui::Checkbox("Noclip", &g_debug.noclip);
        if (ImGui::Button("Give All Weapons + Ammo")) g_debug.give_all_weapons = true;
        ImGui::SameLine();
        if (ImGui::Button("Full Health")) g_debug.player_health = 100;
        ImGui::Separator();
        ImGui::TextDisabled("Inventory (grant items)");
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
            if (ImGui::Button(items[i].label)) g_debug.give_items |= items[i].bit;
            if ((i & 1) == 0) ImGui::SameLine();
        }
        ImGui::NewLine();
        if (ImGui::Button("Give ALL Keys + Tools"))
            for (int i = 0; i < 8; i++) g_debug.give_items |= items[i].bit;
        ImGui::Text("Held:");
        bool any = false;
        for (int i = 0; i < 8; i++) {
            if (g_debug.player_keys & items[i].bit) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "%s", items[i].label);
                any = true;
            }
        }
        if (!any) { ImGui::SameLine(); ImGui::TextDisabled("(none)"); }
    }
    ImGui::End();

    /* ---- Shaders / Post-FX (pretty shaders) ---- */
    ImGui::SetNextWindowPos(ImVec2(650, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 430), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Shaders / Post-FX", nullptr, WF)) {
        PostFX &fx = g_debug.postfx;
        ImGui::Checkbox("Enable post-processing", &fx.enabled);
        ImGui::TextDisabled("Optional eye-candy — off = pixel-faithful.");
        ImGui::Separator();

        static const char *presets[] = {
            "Off", "CRT (arcade)", "Cinematic", "Vibrant", "Custom" };
        int pr = fx.preset;
        if (pr < 0 || pr >= POST_PRESET_COUNT) pr = 0;
        ImGui::TextUnformatted("Preset");
        if (ImGui::Combo("##preset", &pr, presets, POST_PRESET_COUNT)) {
            if (pr == POST_PRESET_OFF) {
                fx.enabled = false; fx.preset = POST_PRESET_OFF;
            } else if (pr != POST_PRESET_CUSTOM) {
                postfx_apply_preset(&fx, pr);   /* turns fx.enabled on */
            } else {
                fx.preset = POST_PRESET_CUSTOM;
            }
        }
        ImGui::Separator();
        ImGui::BeginDisabled(!fx.enabled);

        /* Any manual edit switches the preset label to "Custom". */
        bool edited = false;
        ImGui::TextDisabled("Effects");
        edited |= ImGui::Checkbox("CRT (curve+scanlines+mask)", &fx.crt);
        if (fx.crt) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Curvature", &fx.curvature, 0.0f, 0.3f);
            edited |= ImGui::SliderFloat("Scanlines", &fx.scanline, 0.0f, 1.0f);
            edited |= ImGui::SliderFloat("Aperture mask", &fx.mask, 0.0f, 1.0f);
            ImGui::Unindent();
        }
        edited |= ImGui::Checkbox("Bloom / glow", &fx.bloom);
        if (fx.bloom) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Intensity", &fx.bloom_amt, 0.0f, 2.0f);
            edited |= ImGui::SliderFloat("Threshold", &fx.bloom_thresh, 0.0f, 1.0f);
            ImGui::Unindent();
        }
        edited |= ImGui::Checkbox("Chromatic aberration", &fx.chromatic);
        if (fx.chromatic) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Amount (px)", &fx.chroma_amt, 0.0f, 6.0f);
            ImGui::Unindent();
        }
        edited |= ImGui::Checkbox("Vignette", &fx.vignette);
        if (fx.vignette) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Strength", &fx.vignette_amt, 0.0f, 1.5f);
            ImGui::Unindent();
        }
        edited |= ImGui::Checkbox("Film grain", &fx.grain);
        if (fx.grain) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Grain", &fx.grain_amt, 0.0f, 0.25f);
            ImGui::Unindent();
        }
        edited |= ImGui::Checkbox("Colour grade", &fx.grade);
        if (fx.grade) {
            ImGui::Indent();
            edited |= ImGui::SliderFloat("Saturation", &fx.saturation, 0.0f, 2.0f);
            edited |= ImGui::SliderFloat("Contrast", &fx.contrast, 0.5f, 2.0f);
            edited |= ImGui::SliderFloat("Gamma", &fx.gamma, 0.5f, 2.5f);
            ImGui::Unindent();
        }
        if (edited) fx.preset = POST_PRESET_CUSTOM;

        ImGui::EndDisabled();
    }
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} /* extern "C" */

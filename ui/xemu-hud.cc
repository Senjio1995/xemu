/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "xemu-hud.h"
#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include "xemu-shaders.h"
#include "xemu-snapshots.h"
#include "xemu-custom-widgets.h"
#include "xemu-monitor.h"
#include "xemu-version.h"
#include "xemu-net.h"
#include "xemu-os-utils.h"
#include "xemu-xbe.h"
#include "xemu-reporting.h"

#if defined(_WIN32)
#include "xemu-update.h"
#endif

#include "data/roboto_medium.ttf.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "implot/implot.h"

extern "C" {
#include "noc_file_dialog.h"

// Include necessary QEMU headers
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "migration/snapshot.h"
#include "hw/xbox/mcpx/apu_debug.h"
#include "hw/xbox/nv2a/debug.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "net/pcap.h"

#undef typename
#undef atomic_fetch_add
#undef atomic_fetch_and
#undef atomic_fetch_xor
#undef atomic_fetch_or
#undef atomic_fetch_sub
}

ImFont *g_fixed_width_font;
float g_main_menu_height;
float g_ui_scale = 1.0;
bool g_trigger_style_update = true;

class NotificationManager
{
private:
    const int kNotificationDuration = 4000;
    std::deque<const char *> notification_queue;
    bool active;
    uint32_t notification_end_ts;
    const char *msg;

public:
    NotificationManager()
    {
        active = false;
    }

    ~NotificationManager()
    {

    }

    void QueueNotification(const char *msg)
    {
        notification_queue.push_back(strdup(msg));
    }

    void Draw()
    {
        uint32_t now = SDL_GetTicks();

        if (active) {
            // Currently displaying a notification
            float t = (notification_end_ts - now)/(float)kNotificationDuration;
            if (t > 1.0) {
                // Notification delivered, free it
                free((void*)msg);
                active = false;
            } else {
                // Notification should be displayed
                DrawNotification(t, msg);
            }
        } else {
            // Check to see if a notification is pending
            if (notification_queue.size() > 0) {
                msg = notification_queue[0];
                active = true;
                notification_end_ts = now + kNotificationDuration;
                notification_queue.pop_front();
            }
        }
    }

private:
    void DrawNotification(float t, const char *msg)
    {
        const float DISTANCE = 10.0f;
        static int corner = 1;
        ImGuiIO& io = ImGui::GetIO();
        if (corner != -1)
        {
            ImVec2 window_pos = ImVec2((corner & 1) ? io.DisplaySize.x - DISTANCE : DISTANCE, (corner & 2) ? io.DisplaySize.y - DISTANCE : DISTANCE);
            window_pos.y = g_main_menu_height + DISTANCE;
            ImVec2 window_pos_pivot = ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
        }

        const float fade_in  = 0.1;
        const float fade_out = 0.9;
        float fade = 0;

        if (t < fade_in) {
            // Linear fade in
            fade = t/fade_in;
        } else if (t >= fade_out) {
            // Linear fade out
            fade = 1-(t-fade_out)/(1-fade_out);
        } else {
            // Constant
            fade = 1.0;
        }

        ImVec4 color = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
        color.w *= fade;
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0,0,0,fade*0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, color);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::SetNextWindowBgAlpha(0.90f * fade);
        if (ImGui::Begin("Notification", NULL,
            ImGuiWindowFlags_Tooltip |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoInputs
            ))
        {
            ImGui::Text("%s", msg);
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::End();
    }
};

static void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void Hyperlink(const char *text, const char *url)
{
    // FIXME: Color text when hovered
    ImColor col;
    ImGui::Text("%s", text);
    if (ImGui::IsItemHovered()) {
        col = IM_COL32_WHITE;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
        col = ImColor(127, 127, 127, 255);
    }

    ImVec2 max = ImGui::GetItemRectMax();
    ImVec2 min = ImGui::GetItemRectMin();
    min.x -= 1 * g_ui_scale;
    min.y = max.y;
    max.x -= 1 * g_ui_scale;
    ImGui::GetWindowDrawList()->AddLine(min, max, col, 1.0 * g_ui_scale);

    if (ImGui::IsItemClicked()) {
        xemu_open_web_browser(url);
    }
}

static int PushWindowTransparencySettings(bool transparent, float alpha_transparent = 0.4, float alpha_opaque = 1.0)
{
        float alpha = transparent ? alpha_transparent : alpha_opaque;

        ImVec4 c;

        c = ImGui::GetStyle().Colors[transparent ? ImGuiCol_WindowBg : ImGuiCol_TitleBg];
        c.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_TitleBg, c);

        c = ImGui::GetStyle().Colors[transparent ? ImGuiCol_WindowBg : ImGuiCol_TitleBgActive];
        c.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, c);

        c = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        c.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, c);

        c = ImGui::GetStyle().Colors[ImGuiCol_Border];
        c.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_Border, c);

        c = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
        c.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, c);

        return 5;
}

class MonitorWindow
{
public:
    bool is_open;

private:
    char                  InputBuf[256];
    ImVector<char*>       Items;
    ImVector<const char*> Commands;
    ImVector<char*>       History;
    int                   HistoryPos;    // -1: new line, 0..History.Size-1 browsing history.
    ImGuiTextFilter       Filter;
    bool                  AutoScroll;
    bool                  ScrollToBottom;

public:
    MonitorWindow()
    {
        is_open = false;
        memset(InputBuf, 0, sizeof(InputBuf));
        HistoryPos = -1;
        AutoScroll = true;
        ScrollToBottom = false;
    }
    ~MonitorWindow()
    {
    }

    // Portable helpers
    static int   Stricmp(const char* str1, const char* str2)         { int d; while ((d = toupper(*str2) - toupper(*str1)) == 0 && *str1) { str1++; str2++; } return d; }
    static char* Strdup(const char *str)                             { size_t len = strlen(str) + 1; void* buf = malloc(len); IM_ASSERT(buf); return (char*)memcpy(buf, (const void*)str, len); }
    static void  Strtrim(char* str)                                  { char* str_end = str + strlen(str); while (str_end > str && str_end[-1] == ' ') str_end--; *str_end = 0; }

    void Draw()
    {
        if (!is_open) return;
        int style_pop_cnt = PushWindowTransparencySettings(true);
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 window_pos = ImVec2(0,io.DisplaySize.y/2);
        ImGui::SetNextWindowPos(window_pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y/2), ImGuiCond_Appearing);
        if (ImGui::Begin("Monitor", &is_open, ImGuiWindowFlags_NoCollapse)) {
            const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing(); // 1 separator, 1 input text
            ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar); // Leave room for 1 separator + 1 InputText

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4,1)); // Tighten spacing
            ImGui::PushFont(g_fixed_width_font);
            ImGui::TextUnformatted(xemu_get_monitor_buffer());
            ImGui::PopFont();

            if (ScrollToBottom || (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
                ImGui::SetScrollHereY(1.0f);
            }
            ScrollToBottom = false;

            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::Separator();

            // Command-line
            bool reclaim_focus = ImGui::IsWindowAppearing();

            ImGui::SetNextItemWidth(-1);
            ImGui::PushFont(g_fixed_width_font);
            if (ImGui::InputText("", InputBuf, IM_ARRAYSIZE(InputBuf), ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackCompletion|ImGuiInputTextFlags_CallbackHistory, &TextEditCallbackStub, (void*)this)) {
                char* s = InputBuf;
                Strtrim(s);
                if (s[0])
                    ExecCommand(s);
                strcpy(s, "");
                reclaim_focus = true;
            }
            ImGui::PopFont();

            // Auto-focus on window apparition
            ImGui::SetItemDefaultFocus();
            if (reclaim_focus) {
                ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(style_pop_cnt);
    }

    void toggle_open(void)
    {
        is_open = !is_open;
    }

private:
    void ExecCommand(const char* command_line)
    {
        xemu_run_monitor_command(command_line);

        // Insert into history. First find match and delete it so it can be pushed to the back. This isn't trying to be smart or optimal.
        HistoryPos = -1;
        for (int i = History.Size-1; i >= 0; i--)
            if (Stricmp(History[i], command_line) == 0)
            {
                free(History[i]);
                History.erase(History.begin() + i);
                break;
            }
        History.push_back(Strdup(command_line));

        // On commad input, we scroll to bottom even if AutoScroll==false
        ScrollToBottom = true;
    }

    static int TextEditCallbackStub(ImGuiInputTextCallbackData* data) // In C++11 you are better off using lambdas for this sort of forwarding callbacks
    {
        MonitorWindow* console = (MonitorWindow*)data->UserData;
        return console->TextEditCallback(data);
    }

    int TextEditCallback(ImGuiInputTextCallbackData* data)
    {
        switch (data->EventFlag)
        {
        case ImGuiInputTextFlags_CallbackHistory:
            {
                // Example of HISTORY
                const int prev_history_pos = HistoryPos;
                if (data->EventKey == ImGuiKey_UpArrow)
                {
                    if (HistoryPos == -1)
                        HistoryPos = History.Size - 1;
                    else if (HistoryPos > 0)
                        HistoryPos--;
                }
                else if (data->EventKey == ImGuiKey_DownArrow)
                {
                    if (HistoryPos != -1)
                        if (++HistoryPos >= History.Size)
                            HistoryPos = -1;
                }

                // A better implementation would preserve the data on the current input line along with cursor position.
                if (prev_history_pos != HistoryPos)
                {
                    const char* history_str = (HistoryPos >= 0) ? History[HistoryPos] : "";
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, history_str);
                }
            }
        }
        return 0;
    }
};

class InputWindow
{
public:
    bool is_open;

    InputWindow()
    {
        is_open = false;
    }

    ~InputWindow()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(500.0f*g_ui_scale, 0.0f));
        // Remove window X padding for this window to easily center stuff
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,ImGui::GetStyle().WindowPadding.y));
        if (!ImGui::Begin("Input", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }

        static int active = 0;

        // Output dimensions of texture
        float t_w = 512, t_h = 512;
        // Dimensions of (port+label)s
        float b_x = 0, b_x_stride = 100, b_y = 400;
        float b_w = 68, b_h = 81;
        // Dimensions of controller (rendered at origin)
        float controller_width  = 477.0f;
        float controller_height = 395.0f;

        // Setup rendering to fbo for controller and port images
        ImTextureID id = (ImTextureID)(intptr_t)render_to_fbo(controller_fbo);

        //
        // Render buttons with icons of the Xbox style port sockets with
        // circular numbers above them. These buttons can be activated to
        // configure the associated port, like a tabbed interface.
        //
        ImVec4 color_active(0.50, 0.86, 0.54, 0.12);
        ImVec4 color_inactive(0, 0, 0, 0);

        // Begin a 4-column layout to render the ports
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0,12));
        ImGui::Columns(4, "mixed", false);

        const int port_padding = 8;
        for (int i = 0; i < 4; i++) {
            bool is_currently_selected = (i == active);
            bool port_is_bound = (xemu_input_get_bound(i) != NULL);

            // Set an X offset to center the image button within the column
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(int)((ImGui::GetColumnWidth()-b_w*g_ui_scale-2*port_padding*g_ui_scale)/2));

            // We are using the same texture for all buttons, but ImageButton
            // uses the texture as a unique ID. Push a new ID now to resolve
            // the conflict.
            ImGui::PushID(i);
            float x = b_x+i*b_x_stride;
            ImGui::PushStyleColor(ImGuiCol_Button, is_currently_selected ? color_active : color_inactive);
            bool activated = ImGui::ImageButton(id,
                ImVec2(b_w*g_ui_scale,b_h*g_ui_scale),
                ImVec2(x/t_w, (b_y+b_h)/t_h),
                ImVec2((x+b_w)/t_w, b_y/t_h),
                port_padding);
            ImGui::PopStyleColor();

            if (activated) {
                active = i;
            }

            uint32_t port_color = 0xafafafff;
            bool is_hovered = ImGui::IsItemHovered();
            if (is_currently_selected || port_is_bound) {
                port_color = 0x81dc8a00;
            } else if (is_hovered) {
                port_color = 0x000000ff;
            }

            render_controller_port(x, b_y, i, port_color);

            ImGui::PopID();
            ImGui::NextColumn();
        }
        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::Columns(1);

        //
        // Render input device combo
        //

        // Center the combo above the controller with the same width
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(int)((ImGui::GetColumnWidth()-controller_width*g_ui_scale)/2.0));

        // Note: SetNextItemWidth applies only to the combo element, but not the
        // associated label which follows, so scale back a bit to make space for
        // the label.
        ImGui::SetNextItemWidth(controller_width*0.75*g_ui_scale);

        // List available input devices
        const char *not_connected = "Not Connected";
        ControllerState *bound_state = xemu_input_get_bound(active);

        // Get current controller name
        const char *name;
        if (bound_state == NULL) {
            name = not_connected;
        } else {
            name = bound_state->name;
        }

        if (ImGui::BeginCombo("Input Devices", name))
        {
            // Handle "Not connected"
            bool is_selected = bound_state == NULL;
            if (ImGui::Selectable(not_connected, is_selected)) {
                xemu_input_bind(active, NULL, 1);
                bound_state = NULL;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }

            // Handle all available input devices
            ControllerState *iter;
            QTAILQ_FOREACH(iter, &available_controllers, entry) {
                is_selected = bound_state == iter;
                ImGui::PushID(iter);
                const char *selectable_label = iter->name;
                char buf[128];
                if (iter->bound >= 0) {
                    snprintf(buf, sizeof(buf), "%s (Port %d)", iter->name, iter->bound+1);
                    selectable_label = buf;
                }
                if (ImGui::Selectable(selectable_label, is_selected)) {
                    xemu_input_bind(active, iter, 1);
                    bound_state = iter;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
            }

            ImGui::EndCombo();
        }

        ImGui::Columns(1);

        //
        // Add a separator between input selection and controller graphic
        //
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        //
        // Render controller image
        //
        bool device_selected = false;

        if (bound_state) {
            device_selected = true;
            render_controller(0, 0, 0x81dc8a00, 0x0f0f0f00, bound_state);
        } else {
            static ControllerState state = { 0 };
            render_controller(0, 0, 0x1f1f1f00, 0x0f0f0f00, &state);
        }

        // update_sdl_controller_state(&state);
        // update_sdl_kbd_controller_state(&state);
        ImVec2 cur = ImGui::GetCursorPos();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(int)((ImGui::GetColumnWidth()-controller_width*g_ui_scale)/2.0));
        ImGui::Image(id,
            ImVec2(controller_width*g_ui_scale, controller_height*g_ui_scale),
            ImVec2(0, controller_height/t_h),
            ImVec2(controller_width/t_w, 0));

        if (!device_selected) {
            // ImGui::SameLine();
            const char *msg = "Please select an available input device";
            ImVec2 dim = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPosX(cur.x + (controller_width*g_ui_scale-dim.x)/2);
            ImGui::SetCursorPosY(cur.y + (controller_height*g_ui_scale-dim.y)/2);
            ImGui::Text("%s", msg);
            ImGui::SameLine();
        }

        ImGui::End();
        ImGui::PopStyleVar(); // Window padding

        // Restore original framebuffer target
        render_to_default_fb();
    }
};

static const char *paused_file_open(int flags,
                                    const char *filters,
                                    const char *default_path,
                                    const char *default_name)
{
    bool is_running = runstate_is_running();
    if (is_running) {
        vm_stop(RUN_STATE_PAUSED);
    }
    const char *r = noc_file_dialog_open(flags, filters, default_path, default_name);
    if (is_running) {
        vm_start();
    }

    return r;
}

#define MAX_STRING_LEN 2048 // FIXME: Completely arbitrary and only used here
                            // to give a buffer to ImGui for each field

class SettingsWindow
{
public:
    bool is_open;

private:
    bool dirty;
    bool pending_restart;

    char flashrom_path[MAX_STRING_LEN];
    char bootrom_path[MAX_STRING_LEN];
    char hdd_path[MAX_STRING_LEN];
    char eeprom_path[MAX_STRING_LEN];

public:
    SettingsWindow()
    {
        is_open = false;
        dirty = false;
        pending_restart = false;

        flashrom_path[0] = '\0';
        bootrom_path[0] = '\0';
        hdd_path[0] = '\0';
        eeprom_path[0] = '\0';
    }

    ~SettingsWindow()
    {
    }

    void Load()
    {
        strncpy(flashrom_path, g_config.sys.files.flashrom_path, sizeof(flashrom_path)-1);
        strncpy(bootrom_path, g_config.sys.files.bootrom_path, sizeof(bootrom_path)-1);
        strncpy(hdd_path, g_config.sys.files.hdd_path, sizeof(hdd_path)-1);
        strncpy(eeprom_path, g_config.sys.files.eeprom_path, sizeof(eeprom_path)-1);
        dirty = false;
    }

    void Save()
    {
        xemu_settings_set_string(&g_config.sys.files.flashrom_path, flashrom_path);
        xemu_settings_set_string(&g_config.sys.files.bootrom_path, bootrom_path);
        xemu_settings_set_string(&g_config.sys.files.hdd_path, hdd_path);
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, eeprom_path);
        xemu_queue_notification("Settings saved. Restart to apply updates.");
        pending_restart = true;
        g_config.general.show_welcome = false;
    }

    void FilePicker(const char *name, char *buf, size_t len, const char *filters)
    {
        ImGui::PushID(name);
        if (ImGui::InputText("", buf, len)) {
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(100*g_ui_scale, 0))) {
            const char *selected = paused_file_open(NOC_FILE_DIALOG_OPEN, filters, buf, NULL);
            if ((selected != NULL) && (strcmp(buf, selected) != 0)) {
                strncpy(buf, selected, len-1);
                dirty = true;
            }
        }
        ImGui::PopID();
    }

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(550.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("Settings", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        if (ImGui::IsWindowAppearing()) {
            Load();
        }

        const char *rom_file_filters = ".bin Files\0*.bin\0.rom Files\0*.rom\0All Files\0*.*\0";
        const char *qcow_file_filters = ".qcow2 Files\0*.qcow2\0All Files\0*.*\0";

        ImGui::Columns(2, "", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth()*0.25);

        ImGui::Text("Flash (BIOS) File");
        ImGui::NextColumn();
        float picker_width = ImGui::GetColumnWidth()-120*g_ui_scale;
        ImGui::SetNextItemWidth(picker_width);
        FilePicker("###Flash", flashrom_path, sizeof(flashrom_path), rom_file_filters);
        ImGui::NextColumn();

        ImGui::Text("MCPX Boot ROM File");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(picker_width);
        FilePicker("###BootROM", bootrom_path, sizeof(bootrom_path), rom_file_filters);
        ImGui::NextColumn();

        ImGui::Text("Hard Disk Image File");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(picker_width);
        FilePicker("###HDD", hdd_path, sizeof(hdd_path), qcow_file_filters);
        ImGui::NextColumn();

        ImGui::Text("EEPROM File");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(picker_width);
        FilePicker("###EEPROM", eeprom_path, sizeof(eeprom_path), rom_file_filters);
        ImGui::NextColumn();

        ImGui::Text("System Memory");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth()*0.5);
        ImGui::Combo("###mem", &g_config.sys.mem_limit, "64 MiB\0" "128 MiB\0");
        ImGui::NextColumn();

        ImGui::Dummy(ImVec2(0,0));
        ImGui::NextColumn();
        ImGui::Checkbox("Skip startup animation", &g_config.general.misc.skip_boot_anim);
        ImGui::NextColumn();

#if defined(_WIN32)
        ImGui::Dummy(ImVec2(0,0));
        ImGui::NextColumn();
        ImGui::Checkbox("Check for updates on startup", &g_config.general.updates.check);
        ImGui::NextColumn();
#endif

        ImGui::Columns(1);

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        Hyperlink("Help", "https://xemu.app/docs/getting-started/");
        ImGui::SameLine();

        const char *msg = NULL;
        if (dirty) {
            msg = "Warning: Unsaved changes!";
        } else if (pending_restart) {
            msg = "Restart to apply updates";
        }

        if (msg) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2.0);
            ImGui::Text("%s", msg);
            ImGui::SameLine();
        }

        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-(120+10)*g_ui_scale);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Save", ImVec2(120*g_ui_scale, 0))) {
            Save();
            dirty = false;
            pending_restart = true;
        }

        ImGui::End();
    }
};

static const char *get_os_platform(void) 
{
    const char *platform_name;

#if defined(__linux__)
    platform_name = "Linux";
#elif defined(_WIN32)
    platform_name = "Windows";
#elif defined(__APPLE__)
    platform_name = "macOS";
#else
    platform_name = "Unknown";
#endif
    return platform_name;
}

#ifndef _WIN32
#ifdef CONFIG_CPUID_H
#include <cpuid.h>
#endif
#endif

const char *xemu_get_cpu_info(void)
{
    const char *cpu_info = "";
#ifdef CONFIG_CPUID_H
    static uint32_t brand[12];
    if (__get_cpuid_max(0x80000004, NULL)) {
        __get_cpuid(0x80000002, brand+0x0, brand+0x1, brand+0x2, brand+0x3);
        __get_cpuid(0x80000003, brand+0x4, brand+0x5, brand+0x6, brand+0x7);
        __get_cpuid(0x80000004, brand+0x8, brand+0x9, brand+0xa, brand+0xb);
    }
    cpu_info = (const char *)brand;
#endif
    // FIXME: Support other architectures (e.g. ARM)
    return cpu_info;
}

class SnapshotWindow
{
private:
    QEMUSnapshotInfo *snapshots;
    XemuSnapshotData *extra_data;
    char *fn_key_bindings[8];
    char *current_title_name;
    uint32_t current_title_id;
    int snapshots_len, selected_snapshot, prev_snapshots_len;
    GLuint *thumbnail_tex;

public:
    bool is_open;
    GRegex *search_regex;
    char search_buf[40];

    void Load()
    {
        Error *err = NULL;

        snapshots_len = xemu_snapshots_list(&snapshots, &extra_data, &err);
        if (err) {
            error_reportf_err(err, "failed to list snapshots ");
            snapshots_len = 0;
        }

        if (prev_snapshots_len != snapshots_len) {
            if (prev_snapshots_len > 0) {
                glDeleteTextures(prev_snapshots_len, thumbnail_tex);
                g_free(thumbnail_tex);
            }

            thumbnail_tex = (GLuint*) g_malloc(snapshots_len * sizeof(GLuint));
            glGenTextures(snapshots_len, thumbnail_tex);
        }

        prev_snapshots_len = snapshots_len;

        if (selected_snapshot >= snapshots_len) {
            selected_snapshot = -1;
        }

        struct xbe *xbe = xemu_get_xbe_info();
        if (xbe && xbe->cert->m_titleid != current_title_id) {
            g_free(current_title_name);
            current_title_name = g_utf16_to_utf8(xbe->cert->m_title_name, 40, NULL, NULL, NULL);
            current_title_id = xbe->cert->m_titleid;
        }
    }

    SnapshotWindow()
    {
        is_open = false;
        selected_snapshot = -1;
        prev_snapshots_len = 0;
        xemu_snapshots_mark_dirty();

        search_regex = NULL;

        char buf[10];
        for (size_t i = 0; i < 8; ++i) {
            snprintf(buf, sizeof(buf), "save%ld", i);
            fn_key_bindings[i] = g_strdup(buf);
        }
    }

    ~SnapshotWindow()
    {
        g_free(snapshots);
        g_free(extra_data);
        xemu_snapshots_mark_dirty();

        g_free(current_title_name);
        g_free(search_regex);

        for (size_t i = 0; i < 8; ++i) {
            g_free(fn_key_bindings[i]);
        }
    }

    bool BindFnKey(size_t fn_key, bool unbind)
    {
        assert(fn_key < 8);
        if (!is_open) return false;
        if (selected_snapshot < 0) return true;
        char buf[10];

        if (unbind) {
            snprintf(buf, sizeof(buf), "save%ld", fn_key);
            g_free(fn_key_bindings[fn_key]);
            fn_key_bindings[fn_key] = g_strdup(buf);
            return true;
        }

        g_free(fn_key_bindings[fn_key]);
        fn_key_bindings[fn_key] = g_strdup(snapshots[selected_snapshot].name);

        for (size_t i = 0; i < 8; ++i) {
            if (i == fn_key) continue;
            if (strcmp(fn_key_bindings[i], fn_key_bindings[fn_key]) == 0) {
                snprintf(buf, sizeof(buf), "save%ld", i);
                g_free(fn_key_bindings[i]);
                fn_key_bindings[i] = g_strdup(buf);
            }
        }

        return true;
    }

    char *GetFnKeyBinding(size_t fn_key)
    {
        assert(fn_key < 8);

        /* Prevent loading of filtered snapshots by fn key */
        for (int i = 0; i < snapshots_len; ++i) {
            if (extra_data[i].xbe_title_present && 
                strcmp(current_title_name, extra_data[i].xbe_title) != 0 &&
                strcmp(fn_key_bindings[fn_key], snapshots[i].name) == 0
                ) {
                return NULL;
            }
        }

        return fn_key_bindings[fn_key];
    }

    void DrawSnapshotEntry(int i, float width)
    {
        Error *err = NULL;
        char id[14];
        snprintf(id, sizeof(id), "##sn%d", i);

        ImVec2 text_size = ImGui::CalcTextSize(id, NULL, true);
        ImVec2 cursor = ImGui::GetCursorPos();
        ImVec2 text_cursor = ImGui::GetCursorPos();
        text_cursor.x += text_size.y * 4 + 2;
        if (ImGui::Selectable(id, i == selected_snapshot, 0, ImVec2(0, text_size.y * 4))) {
            if (selected_snapshot == i) {
                xemu_snapshots_load(snapshots[i].name, &err);
                if (err) {
                    error_reportf_err(err, "loadvm: ");
                }
            }
            selected_snapshot = i;
        }

        if (extra_data[i].thumbnail_present) {
            ImGui::SetCursorPos(cursor);
            xemu_snapshots_render_thumbnail(thumbnail_tex[i], &extra_data[i].thumbnail);
            ImGui::Image((void*)(intptr_t)(thumbnail_tex[i]), ImVec2(4 * text_size.y, 4 * text_size.y));
        }

        ImGui::SetCursorPos(text_cursor);
        ImGui::Text(snapshots[i].name);

        for (size_t fkey = 0; fkey < 8; ++fkey) {
            if (strcmp(fn_key_bindings[fkey], snapshots[i].name) == 0) {
                ImGui::SameLine();
                ImGui::Text("(F%d)", fkey + 1);
            }
        }

        g_autoptr(GDateTime) date = g_date_time_new_from_unix_local(snapshots[i].date_sec);
        char *date_buf = g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
        ImGui::SetCursorPos(ImVec2(text_cursor.x, text_cursor.y + text_size.y));
        ImGui::Text(date_buf);
        g_free(date_buf);

        ImGui::SetCursorPos(ImVec2(text_cursor.x, text_cursor.y + 2 * text_size.y));
        if (extra_data[i].xbe_title_present) {
            ImGui::Text(extra_data[i].xbe_title);
        } else {
            ImGui::Text("Unknown");
        }

        ImGui::SetCursorPos(ImVec2(text_cursor.x, text_cursor.y + 3 * text_size.y));
        ImGui::Dummy(ImVec2(0, 1 * text_size.y));
    }

    void DrawSideBar(float width, float height, float top_cursor)
    {
        Error *err = NULL;
        ImGui::SetCursorPos(ImVec2(0.8*width + 5.0f*g_ui_scale, top_cursor));

        auto thumbnail_width = width * 0.2 - 4*g_ui_scale;
        if (selected_snapshot >= 0 && extra_data[selected_snapshot].thumbnail_present) {
            auto thumbnail_height = thumbnail_width * (float)extra_data[selected_snapshot].thumbnail.height 
                                    / (float)extra_data[selected_snapshot].thumbnail.width;
            xemu_snapshots_render_thumbnail(thumbnail_tex[selected_snapshot], &extra_data[selected_snapshot].thumbnail);
            ImGui::Image((void*)(intptr_t)(thumbnail_tex[selected_snapshot]), ImVec2(thumbnail_width, thumbnail_height));
        } else {
            ImGui::Dummy(ImVec2(thumbnail_width, thumbnail_width*0.75));
        }

        ImGui::SetCursorPosX(0.8*width + 5.0f*g_ui_scale);
        
        if (ImGui::Button("Load", ImVec2(80*g_ui_scale, 0)) && selected_snapshot >= 0) {
            xemu_snapshots_load(snapshots[selected_snapshot].name, &err);
            if (err) {
                error_reportf_err(err, "loadvm: ");
            }
        }

        ImGui::SetCursorPosX(0.8*width + 5.0f*g_ui_scale);
        if (ImGui::Button("Save", ImVec2(80*g_ui_scale, 0)) && selected_snapshot >= 0) {
            xemu_snapshots_save(snapshots[selected_snapshot].name, &err);
            if (err) {
                error_reportf_err(err, "savevm: ");
            }
        }
        
        ImGui::SetCursorPosX(0.8*width + 5.0f*g_ui_scale);
        if (ImGui::Button("Delete", ImVec2(80*g_ui_scale, 0)) && selected_snapshot >= 0) {
            xemu_snapshots_delete(snapshots[selected_snapshot].name, &err);
            if (err) {
                error_reportf_err(err, "delvm: ");
            }
        }
    }

    void DrawTopBar(float width);

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(600.0f*g_ui_scale, 400.0f*g_ui_scale));
        if (!ImGui::Begin("Snapshots", &is_open, ImGuiWindowFlags_NoCollapse | 0))
        {
            ImGui::End();
            return;
        }

        Load();

        // auto top_cursor = ImGui::GetCursorPosY();
        auto width = ImGui::GetWindowWidth() - 10.0f*g_ui_scale;
        auto height = ImGui::GetWindowHeight();

        DrawTopBar(width);

        ImGui::ListBoxHeader("##SnapshotsListBox", ImVec2(width, height - ImGui::GetCursorPosY() - 10.0f*g_ui_scale));
        //ImGui::ListBoxHeader("##SnapshotsListBox", ImVec2(0.8 * width, height - 50.0f*g_ui_scale));
        for (int i = 0; i < snapshots_len; ++i) {
            if (extra_data[i].xbe_title_present) {
                if (strcmp(current_title_name, extra_data[i].xbe_title) != 0) {
                    continue;
                }
            }

            if (search_regex) {
                GMatchInfo *match;
                g_regex_match(search_regex, snapshots[i].name, (GRegexMatchFlags)0, &match);
                if (!g_match_info_matches(match)) {
                    g_free(match);
                    continue;
                }

                g_free(match);
            }


            DrawSnapshotEntry(i, width);
        }
        ImGui::ListBoxFooter();

        //DrawSideBar(width, height, top_cursor);

        ImGui::End();
    }
};

static int SnapshotWindowUpdateSearchBox(ImGuiInputTextCallbackData *data)
{
    GError *gerr = NULL;
    SnapshotWindow *win = (SnapshotWindow*)data->UserData;
    if (strcmp(win->search_buf, data->Buf) != 0) {
        strcpy(win->search_buf, data->Buf);
        if (win->search_regex) g_free(win->search_regex);
        if (data->BufTextLen == 0) {
            win->search_regex = NULL;
            return 0;
        }

        char buf[48];
        snprintf(buf, 48, "(.*)%s(.*)", data->Buf);

        win->search_regex = g_regex_new(buf, (GRegexCompileFlags)0, (GRegexMatchFlags)0, &gerr);
        if (gerr) {
            win->search_regex = NULL;
            return 1;
        } 
    }

    return 0;
}

void SnapshotWindow::DrawTopBar(float width)
{
    Error *err = NULL;
    auto top_cursor = ImGui::GetCursorPos();
    auto button_width = 80.0f*g_ui_scale;
    char input_buf[40];

    ImGui::InputText("Search", input_buf, 40, ImGuiInputTextFlags_CallbackEdit, &SnapshotWindowUpdateSearchBox, this);

    if (ImGui::Button("Load", ImVec2(button_width, 25*g_ui_scale)) && selected_snapshot >= 0) {
        xemu_snapshots_load(snapshots[selected_snapshot].name, &err);
        if (err) {
            error_reportf_err(err, "loadvm: ");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(button_width, 25*g_ui_scale)) && selected_snapshot >= 0) {
        xemu_snapshots_save(snapshots[selected_snapshot].name, &err);
        if (err) {
            error_reportf_err(err, "savevm: ");
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(button_width, 25*g_ui_scale)) && selected_snapshot >= 0) {
        xemu_snapshots_delete(snapshots[selected_snapshot].name, &err);
        if (err) {
            error_reportf_err(err, "delvm: ");
        }
    }

    ImGui::SetCursorPosX(top_cursor.x);
}

class AboutWindow
{
public:
    bool is_open;

private:
    char build_info_text[256];
    char platform_info_text[350];

public:
    AboutWindow()
    {
        snprintf(build_info_text, sizeof(build_info_text),
            "Version:      %s\nBranch:       %s\nCommit:       %s\nDate:         %s",
            xemu_version, xemu_branch, xemu_commit, xemu_date);
    }

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(400.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("About", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        static uint32_t time_start = 0;
        if (ImGui::IsWindowAppearing()) { 
            const char *gl_shader_version = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
            const char *gl_version = (const char*)glGetString(GL_VERSION);
            const char *gl_renderer = (const char*)glGetString(GL_RENDERER);
            const char *gl_vendor = (const char*)glGetString(GL_VENDOR);
             
            snprintf(platform_info_text, sizeof(platform_info_text),
                "CPU:          %s\nOS Platform:  %s\nOS Version:   %s\nManufacturer: %s\n"
                "GPU Model:    %s\nDriver:       %s\nShader:       %s",
                 xemu_get_cpu_info(), get_os_platform(), xemu_get_os_info(), gl_vendor,
                 gl_renderer, gl_version, gl_shader_version);
            // FIXME: Show BIOS/BootROM hash

            time_start = SDL_GetTicks();
        }
        uint32_t now = SDL_GetTicks() - time_start;

        ImGui::SetCursorPosY(ImGui::GetCursorPosY()-50*g_ui_scale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-256*g_ui_scale)/2);

        ImTextureID id = (ImTextureID)(intptr_t)render_to_fbo(logo_fbo);
        float t_w = 256.0;
        float t_h = 256.0;
        float x_off = 0;
        ImGui::Image(id,
            ImVec2((t_w-x_off)*g_ui_scale, t_h*g_ui_scale),
            ImVec2(x_off/t_w, t_h/t_h),
            ImVec2(t_w/t_w, 0));
        if (ImGui::IsItemClicked()) {
            time_start = SDL_GetTicks();
        }
        render_logo(now, 0x42e335ff, 0x42e335ff, 0x00000000);
        render_to_default_fb();
        ImGui::SetCursorPosX(10*g_ui_scale);

        ImGui::SetCursorPosY(ImGui::GetCursorPosY()-100*g_ui_scale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(xemu_version).x)/2);
        ImGui::Text("%s", xemu_version);

        ImGui::SetCursorPosX(10*g_ui_scale);
        ImGui::Dummy(ImVec2(0,20*g_ui_scale));

        const char *msg = "Visit https://xemu.app for more information";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2);
        Hyperlink(msg, "https://xemu.app");

        ImGui::Dummy(ImVec2(0,40*g_ui_scale));

        ImGui::PushFont(g_fixed_width_font);
        ImGui::InputTextMultiline("##build_info", build_info_text, sizeof(build_info_text), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5), ImGuiInputTextFlags_ReadOnly);
        ImGui::InputTextMultiline("##platform_info", platform_info_text, sizeof(platform_info_text), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();

        ImGui::End();
    }
};

class NetworkInterface
{
public:
    std::string pcap_name;
    std::string description;
    std::string friendlyname;

    NetworkInterface(pcap_if_t *pcap_desc, char *_friendlyname = NULL)
    {
        pcap_name = pcap_desc->name;
        description = pcap_desc->description ?: pcap_desc->name;
        if (_friendlyname) {
            char *tmp = g_strdup_printf("%s (%s)", _friendlyname, description.c_str());
            friendlyname = tmp;
            g_free((gpointer)tmp);
        } else {
            friendlyname = description;
        }
    }
};

class NetworkInterfaceManager
{
public:
    std::vector<std::unique_ptr<NetworkInterface>> ifaces;
    NetworkInterface *current_iface;
    bool failed_to_load_lib;

    NetworkInterfaceManager()
    {
        current_iface = NULL;
        failed_to_load_lib = false;
    }

    void refresh(void)
    {
        pcap_if_t *alldevs, *iter;
        char err[PCAP_ERRBUF_SIZE];

        if (xemu_net_is_enabled()) {
            return;
        }

#if defined(_WIN32)
        if (pcap_load_library()) {
            failed_to_load_lib = true;
            return;
        }
#endif

        ifaces.clear();
        current_iface = NULL;

        if (pcap_findalldevs(&alldevs, err)) {
            return;
        }

        for (iter=alldevs; iter != NULL; iter=iter->next) {
#if defined(_WIN32)
            char *friendlyname = get_windows_interface_friendly_name(iter->name);
            ifaces.emplace_back(new NetworkInterface(iter, friendlyname));
            if (friendlyname) {
                g_free((gpointer)friendlyname);
            }
#else
            ifaces.emplace_back(new NetworkInterface(iter));
#endif
            if (!strcmp(g_config.net.pcap.netif, iter->name)) {
                current_iface = ifaces.back().get();
            }
        }

        pcap_freealldevs(alldevs);
    }

    void select(NetworkInterface &iface)
    {
        current_iface = &iface;
        xemu_settings_set_string(&g_config.net.pcap.netif,
                                 iface.pcap_name.c_str());
    }

    bool is_current(NetworkInterface &iface)
    {
        return &iface == current_iface;
    }
};


class NetworkWindow
{
public:
    bool is_open;
    char remote_addr[64];
    char local_addr[64];
    std::unique_ptr<NetworkInterfaceManager> iface_mgr;

    NetworkWindow()
    {
        is_open = false;
    }

    ~NetworkWindow()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(500.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("Network", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        if (ImGui::IsWindowAppearing()) {
            strncpy(remote_addr, g_config.net.udp.remote_addr, sizeof(remote_addr)-1);
            strncpy(local_addr, g_config.net.udp.bind_addr, sizeof(local_addr)-1);
        }

        ImGuiInputTextFlags flg = 0;
        bool is_enabled = xemu_net_is_enabled();
        if (is_enabled) {
            flg |= ImGuiInputTextFlags_ReadOnly;
        }

        ImGui::Columns(2, "", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth()*0.33);

        ImGui::Text("Attached To");
        ImGui::SameLine(); HelpMarker("The network backend which the emulated NIC interacts with");
        ImGui::NextColumn();
        if (is_enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
        int temp_backend = g_config.net.backend; // Temporary to make backend combo read-only (FIXME: surely there's a nicer way)
        ImGui::Combo("##backend", is_enabled ? &temp_backend : &g_config.net.backend, "NAT\0UDP Tunnel\0Bridged Adapter\0");
        if (is_enabled) ImGui::PopStyleVar();
        ImGui::SameLine();
        if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
            HelpMarker("User-mode TCP/IP stack with network address translation");
        } else if (g_config.net.backend == CONFIG_NET_BACKEND_UDP) {
            HelpMarker("Tunnels link-layer traffic to a remote host via UDP");
        } else if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
            HelpMarker("Bridges with a host network interface");
        }
        ImGui::NextColumn();

        if (g_config.net.backend == CONFIG_NET_BACKEND_UDP) {
            ImGui::Text("Remote Host");
            ImGui::SameLine(); HelpMarker("The remote <IP address>:<Port> to forward packets to (e.g. 1.2.3.4:9368)");
            ImGui::NextColumn();
            float w = ImGui::GetColumnWidth()-10*g_ui_scale;
            ImGui::SetNextItemWidth(w);
            if (is_enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
            ImGui::InputText("###remote_host", remote_addr, sizeof(remote_addr), flg);
            if (is_enabled) ImGui::PopStyleVar();
            ImGui::NextColumn();

            ImGui::Text("Local Host");
            ImGui::SameLine(); HelpMarker("The local <IP address>:<Port> to receive packets on (e.g. 0.0.0.0:9368)");
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(w);
            if (is_enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
            ImGui::InputText("###local_host", local_addr, sizeof(local_addr), flg);
            if (is_enabled) ImGui::PopStyleVar();
            ImGui::NextColumn();
        } else if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
            static bool should_refresh = true;
            if (iface_mgr.get() == nullptr) {
                iface_mgr.reset(new NetworkInterfaceManager());
                iface_mgr->refresh();
            }

            if (iface_mgr->failed_to_load_lib) {
#if defined(_WIN32)
                ImGui::Columns(1);
                ImGui::Dummy(ImVec2(0,20*g_ui_scale));
                const char *msg = "WinPcap/npcap library could not be loaded.\n"
                                  "To use this attachment, please install npcap.";
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetColumnWidth() - g_ui_scale*ImGui::CalcTextSize(msg).x)/2);
                ImGui::Text("%s", msg);
                ImGui::Dummy(ImVec2(0,10*g_ui_scale));
                ImGui::SetCursorPosX((ImGui::GetWindowWidth()-120*g_ui_scale)/2);
                if (ImGui::Button("Install npcap", ImVec2(120*g_ui_scale, 0))) {
                    xemu_open_web_browser("https://nmap.org/npcap/");
                }
                ImGui::Dummy(ImVec2(0,10*g_ui_scale));
#endif
            } else {
                ImGui::Text("Network Interface");
                ImGui::SameLine(); HelpMarker("Host network interface to bridge with");
                ImGui::NextColumn();

                float w = ImGui::GetColumnWidth()-10*g_ui_scale;
                ImGui::SetNextItemWidth(w);
                const char *selected_display_name = (
                    iface_mgr->current_iface
                    ? iface_mgr->current_iface->friendlyname.c_str()
                    : g_config.net.pcap.netif
                    );
                if (is_enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
                if (ImGui::BeginCombo("###network_iface", selected_display_name)) {
                    if (should_refresh) {
                        iface_mgr->refresh();
                        should_refresh = false;
                    }
                    int i = 0;
                    for (auto& iface : iface_mgr->ifaces) {
                        bool is_selected = iface_mgr->is_current((*iface));
                        ImGui::PushID(i++);
                        if (ImGui::Selectable(iface->friendlyname.c_str(), is_selected)) {
                            if (!is_enabled) iface_mgr->select((*iface));
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                } else {
                    should_refresh = true;
                }
                if (is_enabled) ImGui::PopStyleVar();

                ImGui::NextColumn();
            }
        }

        ImGui::Columns(1);

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        Hyperlink("Help", "https://xemu.app/docs/networking/");

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-(120+10)*g_ui_scale);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(is_enabled ? "Disable" : "Enable", ImVec2(120*g_ui_scale, 0))) {
            if (!is_enabled) {
                xemu_settings_set_string(&g_config.net.udp.remote_addr, remote_addr);
                xemu_settings_set_string(&g_config.net.udp.bind_addr, local_addr);
                xemu_net_enable();
            } else {
                xemu_net_disable();
            }
        }

        ImGui::End();
    }
};

class CompatibilityReporter
{
public:
    CompatibilityReport report;
    bool dirty;
    bool is_open;
    bool is_xbe_identified;
    bool did_send, send_result;
    char token_buf[512];
    int playability;
    char description[1024];
    std::string serialized_report;

    CompatibilityReporter()
    {
        is_open = false;

        report.token = "";
        report.xemu_version = xemu_version;
        report.xemu_branch = xemu_branch;
        report.xemu_commit = xemu_commit;
        report.xemu_date = xemu_date;

        report.os_platform = get_os_platform();
        report.os_version = xemu_get_os_info();
        report.cpu = xemu_get_cpu_info();
        dirty = true;
        is_xbe_identified = false;
        did_send = send_result = false;
    }

    ~CompatibilityReporter()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        const char *playability_names[] = {
            "Broken",
            "Intro",
            "Starts",
            "Playable",
            "Perfect",
        };

        const char *playability_descriptions[] = {
            "This title crashes very soon after launching, or displays nothing at all.",
            "This title displays an intro sequence, but fails to make it to gameplay.",
            "This title starts, but may crash or have significant issues.",
            "This title is playable, but may have minor issues.",
            "This title is playable from start to finish with no noticable issues."
        };

        ImGui::SetNextWindowContentSize(ImVec2(550.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("Report Compatibility", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        if (ImGui::IsWindowAppearing()) {
            report.gl_vendor = (const char *)glGetString(GL_VENDOR);
            report.gl_renderer = (const char *)glGetString(GL_RENDERER);
            report.gl_version = (const char *)glGetString(GL_VERSION);
            report.gl_shading_language_version = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
            struct xbe *xbe = xemu_get_xbe_info();
            is_xbe_identified = xbe != NULL;
            if (is_xbe_identified) {
                report.SetXbeData(xbe);
            }
            did_send = send_result = false;

            playability = 3; // Playable
            report.compat_rating = playability_names[playability];
            description[0] = '\x00';
            report.compat_comments = description;

            strncpy(token_buf, g_config.general.user_token, sizeof(token_buf)-1);
            report.token = token_buf;

            dirty = true;
        }

        if (!is_xbe_identified) {
            ImGui::TextWrapped(
                "An XBE could not be identified. Please launch an official "
                "Xbox title to submit a compatibility report.");
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
            "If you would like to help improve xemu by submitting a compatibility report for this "
            "title, please select an appropriate playability level, enter a "
            "brief description, then click 'Send'."
            "\n\n"
            "Note: By submitting a report, you acknowledge and consent to "
            "collection, archival, and publication of information as outlined "
            "in 'Privacy Disclosure' below.");

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        ImGui::Columns(2, "", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth()*0.25);

        ImGui::Text("User Token");
        ImGui::SameLine();
        HelpMarker("This is a unique access token used to authorize submission of the report. To request a token, click 'Get Token'.");
        ImGui::NextColumn();
        float item_width = ImGui::GetColumnWidth()*0.75-20*g_ui_scale;
        ImGui::SetNextItemWidth(item_width);
        ImGui::PushFont(g_fixed_width_font);
        if (ImGui::InputText("###UserToken", token_buf, sizeof(token_buf), 0)) {
            report.token = token_buf;
            dirty = true;
        }
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::Button("Get Token")) {
            xemu_open_web_browser("https://reports.xemu.app");
        }
        ImGui::NextColumn();

        ImGui::Text("Playability");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(item_width);
        if (ImGui::Combo("###PlayabilityRating", &playability,
            "Broken\0" "Intro/Menus\0" "Starts\0" "Playable\0" "Perfect\0")) {
            report.compat_rating = playability_names[playability];
            dirty = true;
        }
        ImGui::SameLine();
        HelpMarker(playability_descriptions[playability]);
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Text("Description");
        if (ImGui::InputTextMultiline("###desc", description, sizeof(description), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6), 0)) {
            report.compat_comments = description;
            dirty = true;
        }

        if (ImGui::TreeNode("Report Details")) {
            ImGui::PushFont(g_fixed_width_font);
            if (dirty) {
                serialized_report = report.GetSerializedReport();
                dirty = false;
            }
            ImGui::InputTextMultiline("##build_info", (char*)serialized_report.c_str(), strlen(serialized_report.c_str())+1, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 7), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopFont();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Privacy Disclosure (Please read before submission!)")) {
            ImGui::TextWrapped(
                "By volunteering to submit a compatibility report, basic information about your "
                "computer is collected, including: your operating system version, CPU model, "
                "graphics card/driver information, and details about the title which are "
                "extracted from the executable in memory. The contents of this report can be "
                "seen before submission by expanding 'Report Details'."
                "\n\n"
                "Like many websites, upon submission, the public IP address of your computer is "
                "also recorded with your report. If provided, the identity associated with your "
                "token is also recorded."
                "\n\n"
                "This information will be archived and used to analyze, resolve problems with, "
                "and improve the application. This information may be made publicly visible, "
                "for example: to anyone who wishes to see the playability status of a title, as "
                "indicated by your report.");
            ImGui::TreePop();
        }

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        if (did_send) {
            if (send_result) {
                ImGui::Text("Sent! Thanks.");
            } else {
                ImGui::Text("Error: %s (%d)", report.GetResultMessage().c_str(), report.GetResultCode());
            }
            ImGui::SameLine();
        }

        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-(120+10)*g_ui_scale);

        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Send", ImVec2(120*g_ui_scale, 0))) {
            did_send = true;
            send_result = report.Send();
            if (send_result) {
                is_open = false;
                xemu_settings_set_string(&g_config.general.user_token, token_buf);
            }
        }

        ImGui::End();
    }
};

#include <math.h>

float mix(float a, float b, float t)
{
    return a*(1.0-t) + (b-a)*t;
}

class DebugApuWindow
{
public:
    bool is_open;

    DebugApuWindow()
    {
        is_open = false;
    }

    ~DebugApuWindow()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        ImGui::SetNextWindowContentSize(ImVec2(600.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("Audio Debug", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        const struct McpxApuDebug *dbg = mcpx_apu_get_debug_info();


        ImGui::Columns(2, "", false);
        int now = SDL_GetTicks() % 1000;
        float t = now/1000.0f;
        float freq = 1;
        float v = fabs(sin(M_PI*t*freq));
        float c_active = mix(0.4, 0.97, v);
        float c_inactive = 0.2f;

        int voice_monitor = -1;
        int voice_info = -1;
        int voice_mute = -1;

        // Color buttons, demonstrate using PushID() to add unique identifier in the ID stack, and changing style.
        ImGui::PushFont(g_fixed_width_font);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        for (int i = 0; i < 256; i++)
        {
            if (i % 16) {
                ImGui::SameLine();
            }

            float c, s, h;
            h = 0.6;
            if (dbg->vp.v[i].active) {
                if (dbg->vp.v[i].paused) {
                    c = c_inactive;
                    s = 0.4;
                } else {
                    c = c_active;
                    s = 0.7;
                }
                if (mcpx_apu_debug_is_muted(i)) {
                    h = 1.0;
                }
            } else {
                c = c_inactive;
                s = 0;
            }

            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(h, s, c));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(h, s, 0.8));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(h, 0.8f, 1.0));
            char buf[12];
            snprintf(buf, sizeof(buf), "%02x", i);
            ImGui::Button(buf);
            if (/*dbg->vp.v[i].active &&*/ ImGui::IsItemHovered()) {
                voice_monitor = i;
                voice_info = i;
            }
            if (ImGui::IsItemClicked(1)) {
                voice_mute = i;
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
        }
        ImGui::PopStyleVar(3);
        ImGui::PopFont();

        if (voice_info >= 0) {
            const struct McpxApuDebugVoice *voice = &dbg->vp.v[voice_info];
            ImGui::BeginTooltip();
            bool is_paused = voice->paused;
            ImGui::Text("Voice 0x%x/%d %s", voice_info, voice_info, is_paused ? "(Paused)" : "");
            ImGui::SameLine();
            ImGui::Text(voice->stereo ? "Stereo" : "Mono");

            ImGui::Separator();
            ImGui::PushFont(g_fixed_width_font);

            const char *noyes[2] = { "NO", "YES" };
            ImGui::Text("Stream: %-3s Loop: %-3s Persist: %-3s Multipass: %-3s "
                        "Linked: %-3s",
                        noyes[voice->stream], noyes[voice->loop],
                        noyes[voice->persist], noyes[voice->multipass],
                        noyes[voice->linked]);

            const char *cs[4] = { "1 byte", "2 bytes", "ADPCM", "4 bytes" };
            const char *ss[4] = {
                "Unsigned 8b PCM",
                "Signed 16b PCM",
                "Signed 24b PCM",
                "Signed 32b PCM"
            };

            assert(voice->container_size < 4);
            assert(voice->sample_size < 4);
            ImGui::Text("Container Size: %s, Sample Size: %s, Samples per Block: %d",
                cs[voice->container_size], ss[voice->sample_size], voice->samples_per_block);
            ImGui::Text("Rate: %f (%d Hz)", voice->rate, (int)(48000.0/voice->rate));
            ImGui::Text("EBO=%d CBO=%d LBO=%d BA=%x",
                voice->ebo, voice->cbo, voice->lbo, voice->ba);
            ImGui::Text("Mix: ");
            for (int i = 0; i < 8; i++) {
                if (i == 4) ImGui::Text("     ");
                ImGui::SameLine();
                char buf[64];
                if (voice->vol[i] == 0xFFF) {
                    snprintf(buf, sizeof(buf),
                        "Bin %2d (MUTE) ", voice->bin[i]);
                } else {
                    snprintf(buf, sizeof(buf),
                        "Bin %2d (-%.3f) ", voice->bin[i],
                        (float)((voice->vol[i] >> 6) & 0x3f) +
                        (float)((voice->vol[i] >> 0) & 0x3f) / 64.0);
                }
                ImGui::Text("%-17s", buf);
            }
            ImGui::PopFont();
            ImGui::EndTooltip();
        }

        if (voice_monitor >= 0) {
            mcpx_apu_debug_isolate_voice(voice_monitor);
        } else {
            mcpx_apu_debug_clear_isolations();
        }
        if (voice_mute >= 0) {
            mcpx_apu_debug_toggle_mute(voice_mute);
        }

        ImGui::SameLine();
        ImGui::SetColumnWidth(0, ImGui::GetCursorPosX());
        ImGui::NextColumn();

        ImGui::PushFont(g_fixed_width_font);
        ImGui::Text("Frames:      %04d", dbg->frames_processed);
        ImGui::Text("GP Cycles:   %04d", dbg->gp.cycles);
        ImGui::Text("EP Cycles:   %04d", dbg->ep.cycles);
        bool color = (dbg->utilization > 0.9);
        if (color) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0,0,1));
        ImGui::Text("Utilization: %.2f%%", (dbg->utilization*100));
        if (color) ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::Separator();

        static int mon = 0;
        mon = mcpx_apu_debug_get_monitor();
        if (ImGui::Combo("Monitor", &mon, "AC97\0VP Only\0GP Only\0EP Only\0GP/EP if enabled\0")) {
            mcpx_apu_debug_set_monitor(mon);
        }

        static bool gp_realtime;
        gp_realtime = dbg->gp_realtime;
        if (ImGui::Checkbox("GP Realtime\n", &gp_realtime)) {
            mcpx_apu_debug_set_gp_realtime_enabled(gp_realtime);
        }

        static bool ep_realtime;
        ep_realtime = dbg->ep_realtime;
        if (ImGui::Checkbox("EP Realtime\n", &ep_realtime)) {
            mcpx_apu_debug_set_ep_realtime_enabled(ep_realtime);
        }

        ImGui::Columns(1);
        ImGui::End();
    }
};



// utility structure for realtime plot
struct ScrollingBuffer {
    int MaxSize;
    int Offset;
    ImVector<ImVec2> Data;
    ScrollingBuffer() {
        MaxSize = 2000;
        Offset  = 0;
        Data.reserve(MaxSize);
    }
    void AddPoint(float x, float y) {
        if (Data.size() < MaxSize)
            Data.push_back(ImVec2(x,y));
        else {
            Data[Offset] = ImVec2(x,y);
            Offset =  (Offset + 1) % MaxSize;
        }
    }
    void Erase() {
        if (Data.size() > 0) {
            Data.shrink(0);
            Offset  = 0;
        }
    }
};

class DebugVideoWindow
{
public:
    bool is_open;
    bool transparent;

    DebugVideoWindow()
    {
        is_open = false;
        transparent = false;
    }

    ~DebugVideoWindow()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        float alpha = transparent ? 0.2 : 1.0;
        PushWindowTransparencySettings(transparent, 0.2);
        ImGui::SetNextWindowSize(ImVec2(600.0f*g_ui_scale, 150.0f*g_ui_scale), ImGuiCond_Once);
        if (ImGui::Begin("Video Debug", &is_open)) {

            double x_start, x_end;
            static ImPlotAxisFlags rt_axis = ImPlotAxisFlags_NoTickLabels;
            ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(5,5));
            ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
            static ScrollingBuffer fps;
            static float t = 0;
            if (runstate_is_running()) {
                t += ImGui::GetIO().DeltaTime;
                fps.AddPoint(t, g_nv2a_stats.increment_fps);
            }
            x_start = t - 10.0;
            x_end = t;
            ImPlot::SetNextPlotLimitsX(x_start, x_end, ImGuiCond_Always);
            ImPlot::SetNextPlotLimitsY(0, 65, ImGuiCond_Always);

            float plot_width = 0.5 * (ImGui::GetWindowSize().x -
                                      2 * ImGui::GetStyle().WindowPadding.x -
                                      ImGui::GetStyle().ItemSpacing.x);

            ImGui::SetNextWindowBgAlpha(alpha);
            if (ImPlot::BeginPlot("##ScrollingFPS", NULL, NULL, ImVec2(plot_width,75*g_ui_scale), 0, rt_axis, rt_axis | ImPlotAxisFlags_Lock)) {
                if (fps.Data.size() > 0) {
                    ImPlot::PlotShaded("##fps", &fps.Data[0].x, &fps.Data[0].y, fps.Data.size(), 0, fps.Offset, 2 * sizeof(float));
                    ImPlot::PlotLine("##fps", &fps.Data[0].x, &fps.Data[0].y, fps.Data.size(), fps.Offset, 2 * sizeof(float));
                }
                ImPlot::AnnotateClamped(x_start, 65, ImVec2(0,0), ImPlot::GetLastItemColor(), "FPS: %d", g_nv2a_stats.increment_fps);
                ImPlot::EndPlot();
            }

            ImGui::SameLine();

            x_end = g_nv2a_stats.frame_count;
            x_start = x_end - NV2A_PROF_NUM_FRAMES;

            ImPlot::SetNextPlotLimitsX(x_start, x_end, ImGuiCond_Always);
            ImPlot::SetNextPlotLimitsY(0, 100, ImGuiCond_Always);
            ImPlot::PushStyleColor(ImPlotCol_Line, ImPlot::GetColormapColor(1));
            ImGui::SetNextWindowBgAlpha(alpha);
            if (ImPlot::BeginPlot("##ScrollingMSPF", NULL, NULL, ImVec2(plot_width,75*g_ui_scale), 0, rt_axis, rt_axis | ImPlotAxisFlags_Lock)) {
                ImPlot::PlotShaded("##mspf", &g_nv2a_stats.frame_history[0].mspf, NV2A_PROF_NUM_FRAMES, 0, 1, x_start, g_nv2a_stats.frame_ptr, sizeof(g_nv2a_stats.frame_working));
                ImPlot::PlotLine("##mspf", &g_nv2a_stats.frame_history[0].mspf, NV2A_PROF_NUM_FRAMES, 1, x_start, g_nv2a_stats.frame_ptr, sizeof(g_nv2a_stats.frame_working));
                ImPlot::AnnotateClamped(x_start, 100, ImVec2(0,0), ImPlot::GetLastItemColor(), "MSPF: %d", g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr - 1) % NV2A_PROF_NUM_FRAMES].mspf);
                ImPlot::EndPlot();
            }
            ImPlot::PopStyleColor();

            if (ImGui::TreeNode("Advanced")) {
                ImPlot::SetNextPlotLimitsX(x_start, x_end, ImGuiCond_Always);
                ImPlot::SetNextPlotLimitsY(0, 1500, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(alpha);
                if (ImPlot::BeginPlot("##ScrollingDraws", NULL, NULL, ImVec2(-1,500*g_ui_scale), 0, rt_axis, rt_axis | ImPlotAxisFlags_Lock)) {
                    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
                        ImGui::PushID(i);
                        char title[64];
                        snprintf(title, sizeof(title), "%s: %d",
                            nv2a_profile_get_counter_name(i),
                            nv2a_profile_get_counter_value(i));
                        ImPlot::PushStyleColor(ImPlotCol_Line, ImPlot::GetColormapColor(i));
                        ImPlot::PushStyleColor(ImPlotCol_Fill, ImPlot::GetColormapColor(i));
                        ImPlot::PlotLine(title, &g_nv2a_stats.frame_history[0].counters[i], NV2A_PROF_NUM_FRAMES, 1, x_start, g_nv2a_stats.frame_ptr, sizeof(g_nv2a_stats.frame_working));
                        ImPlot::PopStyleColor(2);
                        ImGui::PopID();
                    }
                    ImPlot::EndPlot();
                }
                ImGui::TreePop();
            }

            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(2)) {
                transparent = !transparent;
            }

            ImPlot::PopStyleVar(2);
        }
        ImGui::End();
        ImGui::PopStyleColor(5);
    }
};

#if defined(_WIN32)
class AutoUpdateWindow
{
protected:
    Updater updater;

public:
    bool is_open;

    AutoUpdateWindow()
    {
        is_open = false;
    }

    ~AutoUpdateWindow()
    {
    }

    void check_for_updates_and_prompt_if_available()
    {
        updater.check_for_update([this](){
            is_open |= updater.is_update_available();
        });
    }

    void Draw()
    {
        if (!is_open) return;
        ImGui::SetNextWindowContentSize(ImVec2(550.0f*g_ui_scale, 0.0f));
        if (!ImGui::Begin("Update", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        if (ImGui::IsWindowAppearing() && !updater.is_update_available()) {
            updater.check_for_update();
        }

        const char *status_msg[] = {
            "",
            "An error has occured. Try again.",
            "Checking for update...",
            "Downloading update...",
            "Update successful! Restart to launch updated version of xemu."
        };
        const char *available_msg[] = {
            "Update availability unknown.",
            "This version of xemu is up to date.",
            "An updated version of xemu is available!",
        };

        if (updater.get_status() == UPDATER_IDLE) {
            ImGui::Text(available_msg[updater.get_update_availability()]);
        } else {
            ImGui::Text(status_msg[updater.get_status()]);
        }

        if (updater.is_updating()) {
            ImGui::ProgressBar(updater.get_update_progress_percentage()/100.0f,
                               ImVec2(-1.0f, 0.0f));
        }

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        float w = (130)*g_ui_scale;
        float bw = w + (10)*g_ui_scale;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-bw);

        if (updater.is_checking_for_update() || updater.is_updating()) {
            if (ImGui::Button("Cancel", ImVec2(w, 0))) {
                updater.cancel();
            }
        } else {
            if (updater.is_pending_restart()) {
                if (ImGui::Button("Restart", ImVec2(w, 0))) {
                    updater.restart_to_updated();
                }
            } else if (updater.is_update_available()) {
                if (ImGui::Button("Update", ImVec2(w, 0))) {
                    updater.update();
                }
            } else {
                if (ImGui::Button("Check for Update", ImVec2(w, 0))) {
                    updater.check_for_update();
                }
            }
        }

        ImGui::End();
    }
};
#endif

static MonitorWindow monitor_window;
static DebugApuWindow apu_window;
static DebugVideoWindow video_window;
static InputWindow input_window;
static NetworkWindow network_window;
static SnapshotWindow save_state_window;
static AboutWindow about_window;
static SettingsWindow settings_window;
static CompatibilityReporter compatibility_reporter_window;
static NotificationManager notification_manager;
#if defined(_WIN32)
static AutoUpdateWindow update_window;
#endif
static std::deque<const char *> g_errors;

#ifdef CONFIG_RENDERDOC
static bool capture_renderdoc_frame = false;
#endif

class FirstBootWindow
{
public:
    bool is_open;

    FirstBootWindow()
    {
        is_open = false;
    }

    ~FirstBootWindow()
    {
    }

    void Draw()
    {
        if (!is_open) return;

        ImVec2 size(400*g_ui_scale, 300*g_ui_scale);
        ImGuiIO& io = ImGui::GetIO();

        ImVec2 window_pos = ImVec2((io.DisplaySize.x - size.x)/2, (io.DisplaySize.y - size.y)/2);
        ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);

        ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
        if (!ImGui::Begin("First Boot", &is_open, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration)) {
            ImGui::End();
            return;
        }

        static uint32_t time_start = 0;
        if (ImGui::IsWindowAppearing()) {
            time_start = SDL_GetTicks();
        }
        uint32_t now = SDL_GetTicks() - time_start;

        ImGui::SetCursorPosY(ImGui::GetCursorPosY()-50*g_ui_scale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-256*g_ui_scale)/2);

        ImTextureID id = (ImTextureID)(intptr_t)render_to_fbo(logo_fbo);
        float t_w = 256.0;
        float t_h = 256.0;
        float x_off = 0;
        ImGui::Image(id,
            ImVec2((t_w-x_off)*g_ui_scale, t_h*g_ui_scale),
            ImVec2(x_off/t_w, t_h/t_h),
            ImVec2(t_w/t_w, 0));
        if (ImGui::IsItemClicked()) {
            time_start = SDL_GetTicks();
        }
        render_logo(now, 0x42e335ff, 0x42e335ff, 0x00000000);
        render_to_default_fb();

        ImGui::SetCursorPosY(ImGui::GetCursorPosY()-100*g_ui_scale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(xemu_version).x)/2);
        ImGui::Text("%s", xemu_version);

        ImGui::SetCursorPosX(10*g_ui_scale);
        ImGui::Dummy(ImVec2(0,20*g_ui_scale));

        const char *msg = "To get started, please configure machine settings.";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2);
        ImGui::Text("%s", msg);

        ImGui::Dummy(ImVec2(0,20*g_ui_scale));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-120*g_ui_scale)/2);
        if (ImGui::Button("Settings", ImVec2(120*g_ui_scale, 0))) {
            settings_window.is_open = true; // FIXME
        }
        ImGui::Dummy(ImVec2(0,20*g_ui_scale));

        msg = "Visit https://xemu.app for more information";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2);
        Hyperlink(msg, "https://xemu.app");

        ImGui::End();
    }
};

static bool is_shortcut_key_pressed(int scancode)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool is_osx = io.ConfigMacOSXBehaviors;
    const bool is_shortcut_key = (is_osx ? (io.KeySuper && !io.KeyCtrl) : (io.KeyCtrl && !io.KeySuper)) && !io.KeyAlt && !io.KeyShift; // OS X style: Shortcuts using Cmd/Super instead of Ctrl
    return is_shortcut_key && io.KeysDown[scancode] && (io.KeysDownDuration[scancode] == 0.0);
}

static bool is_mod_key_down(void)
{
    ImGuiIO& io = ImGui::GetIO();
    return io.KeyShift && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt;
}

static void action_eject_disc(void)
{
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");
    xemu_eject_disc();
}

static void action_load_disc(void)
{
    const char *iso_file_filters = ".iso Files\0*.iso\0All Files\0*.*\0";
    const char *new_disc_path = paused_file_open(NOC_FILE_DIALOG_OPEN, iso_file_filters, g_config.sys.files.dvd_path, NULL);
    if (new_disc_path == NULL) {
        /* Cancelled */
        return;
    }
    xemu_settings_set_string(&g_config.sys.files.dvd_path, new_disc_path);
    xemu_load_disc(new_disc_path);
}

static void action_toggle_pause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    } else {
        vm_start();
    }
}

static void action_reset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void action_shutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}


static bool is_key_pressed(int scancode)
{
    ImGuiIO& io = ImGui::GetIO();
    return io.KeysDown[scancode] && (io.KeysDownDuration[scancode] == 0.0);
}

static void process_keyboard_shortcuts(void)
{
    if (is_shortcut_key_pressed(SDL_SCANCODE_E)) {
        action_eject_disc();
    }

    if (is_shortcut_key_pressed(SDL_SCANCODE_O)) {
        action_load_disc();
    }

    if (is_shortcut_key_pressed(SDL_SCANCODE_P)) {
        action_toggle_pause();
    }

    if (is_shortcut_key_pressed(SDL_SCANCODE_R)) {
        action_reset();
    }

    if (is_shortcut_key_pressed(SDL_SCANCODE_Q)) {
        action_shutdown();
    }

    if (is_key_pressed(SDL_SCANCODE_GRAVE)) {
        monitor_window.toggle_open();
    }

    for (size_t fkey = 0; fkey < 8; fkey++) {
        if (is_key_pressed(SDL_SCANCODE_F1 + fkey)) {
            if (save_state_window.BindFnKey(fkey, is_mod_key_down())) {
                continue;
            }

            char *vm_name = save_state_window.GetFnKeyBinding(fkey);
            if (!vm_name) continue;

            Error *err = NULL;
            if (is_mod_key_down()) {
                xemu_snapshots_save(vm_name, &err);
            } else {
                xemu_snapshots_load(vm_name, &err);
            }

            if (err) {
                error_reportf_err(err, "snapshot: ");
            }
        }
    }

#if defined(DEBUG_NV2A_GL) && defined(CONFIG_RENDERDOC)
    if (is_key_pressed(SDL_SCANCODE_F10)) {
        nv2a_dbg_renderdoc_capture_frames(1);
    }
#endif
}

#if defined(__APPLE__)
#define SHORTCUT_MENU_TEXT(c) "Cmd+" #c
#else
#define SHORTCUT_MENU_TEXT(c) "Ctrl+" #c
#endif

static void ShowMainMenu()
{
    bool running = runstate_is_running();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Machine"))
        {
            if (ImGui::MenuItem("Eject Disc", SHORTCUT_MENU_TEXT(E))) {
                action_eject_disc();
            }
            if (ImGui::MenuItem("Load Disc...", SHORTCUT_MENU_TEXT(O))) {
                action_load_disc();
            }

            ImGui::Separator();

            ImGui::MenuItem("Input",     NULL, &input_window.is_open);
            ImGui::MenuItem("Network",   NULL, &network_window.is_open);
            ImGui::MenuItem("Snapshots", NULL, &save_state_window.is_open);
            ImGui::MenuItem("Settings",  NULL, &settings_window.is_open);

            ImGui::Separator();

            if (ImGui::MenuItem(running ? "Pause" : "Run", SHORTCUT_MENU_TEXT(P))) {
                action_toggle_pause();
            }
            if (ImGui::MenuItem("Reset", SHORTCUT_MENU_TEXT(R))) {
                action_reset();
            }
            if (ImGui::MenuItem("Shutdown", SHORTCUT_MENU_TEXT(Q))) {
                action_shutdown();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            int ui_scale_combo = g_ui_scale - 1.0;
            if (ui_scale_combo < 0) ui_scale_combo = 0;
            if (ui_scale_combo > 1) ui_scale_combo = 1;
            if (ImGui::Combo("UI Scale", &ui_scale_combo, "1x\0" "2x\0")) {
                g_ui_scale = ui_scale_combo + 1;
                g_config.display.ui.scale = g_ui_scale;
                g_trigger_style_update = true;
            }

            int rendering_scale = nv2a_get_surface_scale_factor() - 1;
            if (ImGui::Combo("Rendering Scale", &rendering_scale, "1x\0" "2x\0" "3x\0" "4x\0" "5x\0" "6x\0" "7x\0" "8x\0" "9x\0" "10x\0")) {
                nv2a_set_surface_scale_factor(rendering_scale+1);
            }

            if (ImGui::Combo(
                    "Scaling Mode", &g_config.display.ui.fit, "Center\0Scale\0Scale (Widescreen 16:9)\0Scale (4:3)\0Stretch\0")) {
            }
            ImGui::SameLine(); HelpMarker("Controls how the rendered content should be scaled into the window");
            if (ImGui::MenuItem("Fullscreen", SHORTCUT_MENU_TEXT(Alt+F), xemu_is_fullscreen(), true)) {
                xemu_toggle_fullscreen();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Monitor", "~", &monitor_window.is_open);
            ImGui::MenuItem("Audio", NULL, &apu_window.is_open);
            ImGui::MenuItem("Video", NULL, &video_window.is_open);
#if defined(DEBUG_NV2A_GL) && defined(CONFIG_RENDERDOC)
            if (nv2a_dbg_renderdoc_available()) {
                ImGui::MenuItem("RenderDoc: Capture", NULL, &capture_renderdoc_frame);
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Help", NULL))
            {
                xemu_open_web_browser("https://xemu.app/docs/getting-started/");
            }

            ImGui::MenuItem("Report Compatibility...", NULL, &compatibility_reporter_window.is_open);
#if defined(_WIN32)
            ImGui::MenuItem("Check for Updates...", NULL, &update_window.is_open);
#endif

            ImGui::Separator();
            ImGui::MenuItem("About", NULL, &about_window.is_open);
            ImGui::EndMenu();
        }

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    }
}

static void InitializeStyle()
{
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->Clear();

    ImFontConfig roboto_font_cfg = ImFontConfig();
    roboto_font_cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)roboto_medium_data, roboto_medium_size, 16*g_ui_scale, &roboto_font_cfg);

    ImFontConfig font_cfg = ImFontConfig();
    font_cfg.OversampleH = font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;
    font_cfg.SizePixels = 13.0f*g_ui_scale;
    g_fixed_width_font = io.Fonts->AddFontDefault(&font_cfg);

    ImGui_ImplOpenGL3_CreateFontsTexture();

    ImGuiStyle style;
    style.WindowRounding = 8.0;
    style.FrameRounding = 8.0;
    style.GrabRounding = 12.0;
    style.PopupRounding = 5.0;
    style.ScrollbarRounding = 12.0;
    style.FramePadding.x = 10;
    style.FramePadding.y = 4;
    style.WindowBorderSize = 0;
    style.PopupBorderSize = 0;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    ImGui::GetStyle() = style;
    ImGui::GetStyle().ScaleAllSizes(g_ui_scale);

    // Set default theme, override
    ImGui::StyleColorsDark();

    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.86f, 0.93f, 0.89f, 0.78f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.16f, 0.16f, 0.16f, 0.58f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.16f, 0.16f, 0.16f, 0.90f);
    colors[ImGuiCol_Border]                 = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.28f, 0.71f, 0.25f, 0.76f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.71f, 0.25f, 0.86f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.28f, 0.71f, 0.25f, 0.86f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    colors[ImGuiCol_PlotLines]              = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.28f, 0.71f, 0.25f, 0.43f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);
}

/* External interface, called from ui/xemu.c which handles SDL main loop */
static FirstBootWindow first_boot_window;
static SDL_Window *g_sdl_window;

void xemu_hud_init(SDL_Window* window, void* sdl_gl_context)
{
    xemu_monitor_init();

    initialize_custom_ui_rendering();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL2_InitForOpenGL(window, sdl_gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");

    first_boot_window.is_open = g_config.general.show_welcome;

    int ui_scale_int = g_config.display.ui.scale;
    if (ui_scale_int < 1) ui_scale_int = 1;
    g_ui_scale = ui_scale_int;

    g_sdl_window = window;

    ImPlot::CreateContext();

#if defined(_WIN32)
    if (!g_config.general.show_welcome && g_config.general.updates.check) {
        update_window.check_for_updates_and_prompt_if_available();
    }
#endif
}

void xemu_hud_cleanup(void)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    ImGui_ImplSDL2_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_render(void)
{
    uint32_t now = SDL_GetTicks();
    bool ui_wakeup = false;

    // Combine all controller states to allow any controller to navigate
    uint32_t buttons = 0;
    int16_t axis[CONTROLLER_AXIS__COUNT] = {0};

    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type != INPUT_DEVICE_SDL_GAMECONTROLLER) continue;
        buttons |= iter->buttons;
        // We simply take any axis that is >10 % activation
        for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
            if ((iter->axis[i] > 3276) || (iter->axis[i] < -3276)) {
                axis[i] = iter->axis[i];
            }
        }
    }

    // If the guide button is pressed, wake the ui
    bool menu_button = false;
    if (buttons & CONTROLLER_BUTTON_GUIDE) {
        ui_wakeup = true;
        menu_button = true;
    }

    // Allow controllers without a guide button to also work
    if ((buttons & CONTROLLER_BUTTON_BACK) &&
        (buttons & CONTROLLER_BUTTON_START)) {
        ui_wakeup = true;
        menu_button = true;
    }

    // If the mouse is moved, wake the ui
    static ImVec2 last_mouse_pos = ImVec2();
    ImVec2 current_mouse_pos = ImGui::GetMousePos();
    if ((current_mouse_pos.x != last_mouse_pos.x) ||
        (current_mouse_pos.y != last_mouse_pos.y)) {
        last_mouse_pos = current_mouse_pos;
        ui_wakeup = true;
    }

    // If mouse capturing is enabled (we are in a dialog), ensure the UI is alive
    bool controller_focus_capture = false;
    ImGuiIO& io = ImGui::GetIO();
    if (io.NavActive) {
        ui_wakeup = true;
        controller_focus_capture = true;
    }

    // Prevent controller events from going to the guest if they are being used
    // to navigate the HUD
    xemu_input_set_test_mode(controller_focus_capture);

    if (g_trigger_style_update) {
        InitializeStyle();
        g_trigger_style_update = false;
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();

    // Override SDL2 implementation gamecontroller interface
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL2_NewFrame(g_sdl_window);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    // Update gamepad inputs (from imgui_impl_sdl.cpp)
    memset(io.NavInputs, 0, sizeof(io.NavInputs));
    #define MAP_BUTTON(NAV_NO, BUTTON_NO)       { io.NavInputs[NAV_NO] = (buttons & BUTTON_NO) ? 1.0f : 0.0f; }
    #define MAP_ANALOG(NAV_NO, AXIS_NO, V0, V1) { float vn = (float)(axis[AXIS_NO] - V0) / (float)(V1 - V0); if (vn > 1.0f) vn = 1.0f; if (vn > 0.0f && io.NavInputs[NAV_NO] < vn) io.NavInputs[NAV_NO] = vn; }
    const int thumb_dead_zone = 8000;           // SDL_gamecontroller.h suggests using this value.
    MAP_BUTTON(ImGuiNavInput_Activate,      CONTROLLER_BUTTON_A);               // Cross / A
    MAP_BUTTON(ImGuiNavInput_Cancel,        CONTROLLER_BUTTON_B);               // Circle / B
    MAP_BUTTON(ImGuiNavInput_Input,         CONTROLLER_BUTTON_Y);               // Triangle / Y
    MAP_BUTTON(ImGuiNavInput_DpadLeft,      CONTROLLER_BUTTON_DPAD_LEFT);       // D-Pad Left
    MAP_BUTTON(ImGuiNavInput_DpadRight,     CONTROLLER_BUTTON_DPAD_RIGHT);      // D-Pad Right
    MAP_BUTTON(ImGuiNavInput_DpadUp,        CONTROLLER_BUTTON_DPAD_UP);         // D-Pad Up
    MAP_BUTTON(ImGuiNavInput_DpadDown,      CONTROLLER_BUTTON_DPAD_DOWN);       // D-Pad Down
    MAP_BUTTON(ImGuiNavInput_FocusPrev,     CONTROLLER_BUTTON_WHITE);           // L1 / LB
    MAP_BUTTON(ImGuiNavInput_FocusNext,     CONTROLLER_BUTTON_BLACK);           // R1 / RB
    MAP_BUTTON(ImGuiNavInput_TweakSlow,     CONTROLLER_BUTTON_WHITE);           // L1 / LB
    MAP_BUTTON(ImGuiNavInput_TweakFast,     CONTROLLER_BUTTON_BLACK);           // R1 / RB

    // Allow Guide and "Back+Start" buttons to act as Menu button
    if (menu_button) {
        io.NavInputs[ImGuiNavInput_Menu] = 1.0;
    }

    MAP_ANALOG(ImGuiNavInput_LStickLeft,    CONTROLLER_AXIS_LSTICK_X, -thumb_dead_zone, -32768);
    MAP_ANALOG(ImGuiNavInput_LStickRight,   CONTROLLER_AXIS_LSTICK_X, +thumb_dead_zone, +32767);
    MAP_ANALOG(ImGuiNavInput_LStickUp,      CONTROLLER_AXIS_LSTICK_Y, +thumb_dead_zone, +32767);
    MAP_ANALOG(ImGuiNavInput_LStickDown,    CONTROLLER_AXIS_LSTICK_Y, -thumb_dead_zone, -32767);

    ImGui::NewFrame();
    process_keyboard_shortcuts();

#if defined(DEBUG_NV2A_GL) && defined(CONFIG_RENDERDOC)
    if (capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1);
        capture_renderdoc_frame = false;
    }
#endif

    bool show_main_menu = true;

    if (first_boot_window.is_open) {
        show_main_menu = false;
    }

    if (show_main_menu) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        if (ui_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1.0);
            alpha = 1.0-t;
            if (t >= 1.0) {
                alpha = 0.0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    first_boot_window.Draw();
    input_window.Draw();
    settings_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    save_state_window.Draw();
    about_window.Draw();
    network_window.Draw();
    compatibility_reporter_window.Draw();
    notification_manager.Draw();
#if defined(_WIN32)
    update_window.Draw();
#endif

    // Very rudimentary error notification API
    if (g_errors.size() > 0) {
        ImGui::OpenPopup("Error");
    }
    if (ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", g_errors[0]);
        ImGui::Dummy(ImVec2(0,16));
        ImGui::SetItemDefaultFocus();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-(120+10));
        if (ImGui::Button("Ok", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            free((void*)g_errors[0]);
            g_errors.pop_front();
        }
        ImGui::EndPopup();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/* External interface, exposed via xemu-notifications.h */

void xemu_queue_notification(const char *msg)
{
    notification_manager.QueueNotification(msg);
}

void xemu_queue_error_message(const char *msg)
{
    g_errors.push_back(strdup(msg));
}

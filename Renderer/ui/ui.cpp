#include "ui.h"
#include "../graphics.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <globals.h>
#include <managers/logging/logging.h>
#include <RBX/task_scheduler/task_scheduler.hpp>

std::deque<module::core::graphics::ui::ui_instruction_t> module::core::graphics::ui::ui_queue;

static std::unordered_map<std::string, bool> checkbox_states;
static std::unordered_map<std::string, float> slider_states;
static std::unordered_map<std::string, int> combo_states;
static std::unordered_map<std::string, int> listbox_states;

std::vector<module::core::graphics::ui::editor_tab_t> module::core::graphics::ui::c_ui::editor_tabs;
int module::core::graphics::ui::c_ui::active_tab = 0;
int module::core::graphics::ui::c_ui::tab_counter = 0;
int module::core::graphics::ui::c_ui::current_script = 0;

namespace
{
    struct RobloxLogLine
    {
        std::string text;
        ImU32 color;
        bool problem;
    };

    static std::vector<RobloxLogLine> g_roblox_lines;
    static std::filesystem::file_time_type g_roblox_log_stamp{};
    static time_t g_roblox_last_check = 0;

    // Read the live Roblox player log so the Roblox Output / Problems tabs show
    // what the client really printed, not fabricated text.
    static void refresh_roblox_output()
    {
        const time_t now = time(nullptr);
        if (now - g_roblox_last_check < 1)
            return;
        g_roblox_last_check = now;

        char appdata[MAX_PATH]{};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH))
            return;
        const std::filesystem::path dir = std::filesystem::path(appdata) / "Roblox" / "logs";
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            return;

        std::filesystem::path newest;
        std::filesystem::file_time_type newest_time{};
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            const auto name = entry.path().filename().string();
            if (name.find("Player") == std::string::npos ||
                name.find("_last.log") == std::string::npos ||
                name.find("CrashHandler") != std::string::npos)
                continue;
            const auto wt = entry.last_write_time(ec);
            if (!newest.empty() && wt <= newest_time)
                continue;
            newest = entry.path();
            newest_time = wt;
        }
        if (newest.empty() || (newest_time == g_roblox_log_stamp && !g_roblox_lines.empty()))
            return;
        g_roblox_log_stamp = newest_time;

        std::ifstream file(newest);
        std::vector<RobloxLogLine> lines;
        std::string line;
        while (std::getline(file, line))
        {
            const size_t tag = line.find("[FLog::");
            if (tag == std::string::npos)
                continue;
            // Roblox's real Output panel only shows script print/warn/error and
            // FLog::Output - not asset/telemetry/HTTP noise from the log file.
            static const char* kOutputTags[] = {
                "[FLog::Output]", "[FLog::CreatorOutput]",
                "[FLog::CreatorWarning]", "[FLog::CreatorError]"
            };
            bool isOutputTag = false;
            for (const char* t : kOutputTags)
            {
                if (line.compare(tag, strlen(t), t) == 0)
                {
                    isOutputTag = true;
                    break;
                }
            }
            if (!isOutputTag)
                continue;
            const size_t close = line.find(']', tag);
            if (close == std::string::npos || close + 1 >= line.size())
                continue;
            std::string msg = line.substr(close + 1);
            while (!msg.empty() && msg.front() == ' ')
                msg.erase(msg.begin());

            const bool isError = line.find("[FLog::CreatorError]") != std::string::npos ||
                msg.find("Error:") == 0 || msg.find("error") == 0;
            const bool isWarning = line.find("[FLog::CreatorWarning]") != std::string::npos ||
                line.find("[FLog::Warning]") != std::string::npos;
            const ImU32 color = isError ? IM_COL32(255, 110, 110, 255)
                : isWarning ? IM_COL32(235, 190, 90, 255)
                : IM_COL32(150, 150, 155, 255);
            lines.push_back({ msg, color, isError || isWarning });
        }
        if (lines.size() > 400)
            lines.erase(lines.begin(), lines.begin() + (lines.size() - 400));
        g_roblox_lines = std::move(lines);
    }

    static std::string clean_module_log(const std::string& text)
    {
        static const std::string prefix = "[justafatguy67]";
        const size_t at = text.find(prefix);
        return at == std::string::npos ? text : text.substr(at + prefix.size());
    }
}

static const char* script_labels[] = {
    (("InfiniteYield")), (("UNC")), (("Vulnerability Test")), (("Unnamed-ESP")), (("Myriad")), (("Sunc")), (("Cobalt")), (("Dex"))
};

static const char* script_sources[] = {
    ("loadstring(game:HttpGet(\"https://raw.githubusercontent.com/EdgeIY/infiniteyield/master/source\"))()"),
    ("loadstring(game:HttpGet(\"https://raw.githubusercontent.com/unified-naming-convention/NamingStandard/refs/heads/main/UNCCheckEnv.lua\"))()"),
    ("loadstring(game:HttpGet(\"https://raw.githubusercontent.com/zryr/Vulnerability-Check/refs/heads/main/Script\"))()"),
    ("pcall(function() loadstring(game:HttpGet('https://raw.githubusercontent.com/ic3w0lf22/Unnamed-ESP/master/UnnamedESP.lua'))() end)"),
    ("loadstring(game:HttpGetAsync(\"https://github.com/Proton-Utilities/Myriad/releases/latest/download/myriad.luau\"))()"),
    ("getgenv().sUNCDebug = {[\"printcheckpoints\"] = false, [\"delaybetweentests\"] = 0, [\"printtesttimetaken\"] = false} loadstring(game:HttpGet(\"https://script.sunc.su/\"))()"),
    ("loadstring(game:HttpGet(\"https://github.com/notpoiu/cobalt/releases/latest/download/Cobalt.luau\"))()"),
    ("loadstring(game:HttpGet(\"https://raw.githubusercontent.com/infyiff/backup/main/dex.lua\"))()")
};

void module::core::graphics::ui::c_ui::fire_callback_void(int ref)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pcall(L, 0, 0, 0);
}

void module::core::graphics::ui::c_ui::fire_callback_bool(int ref, bool val)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushboolean(L, val);
    lua_pcall(L, 1, 0, 0);
}

void module::core::graphics::ui::c_ui::fire_callback_float(int ref, float val)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushnumber(L, val);
    lua_pcall(L, 1, 0, 0);
}

void module::core::graphics::ui::c_ui::fire_callback_str(int ref, const std::string& val)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushstring(L, val.c_str());
    lua_pcall(L, 1, 0, 0);
}

void module::core::graphics::ui::c_ui::fire_callback_int(int ref, int val)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushinteger(L, val);
    lua_pcall(L, 1, 0, 0);
}

void module::core::graphics::ui::c_ui::fire_callback_color(int ref, float r, float g, float b, float a)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_State* L = globals.exploit_thread;
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    lua_pushnumber(L, a);
    lua_pcall(L, 4, 0, 0);
}

// Syntax palette copied from ui_mockup.html (VS Code Dark+ colours).
static TextEditor::Palette mockup_editor_palette()
{
    TextEditor::Palette p = TextEditor::GetDarkPalette();
    using K = TextEditor::PaletteIndex;
    p[(int)K::Default]            = IM_COL32(171, 178, 191, 255); // #abb2bf
    p[(int)K::Keyword]            = IM_COL32(86, 156, 214, 255);  // #569cd6
    p[(int)K::Number]             = IM_COL32(181, 206, 168, 255);
    p[(int)K::String]             = IM_COL32(206, 145, 120, 255); // #ce9178
    p[(int)K::CharLiteral]        = IM_COL32(206, 145, 120, 255);
    p[(int)K::Punctuation]        = IM_COL32(171, 178, 191, 255);
    p[(int)K::Preprocessor]       = IM_COL32(197, 134, 192, 255);
    p[(int)K::Identifier]         = IM_COL32(156, 220, 254, 255); // #9cdcfe
    p[(int)K::KnownIdentifier]    = IM_COL32(220, 220, 170, 255); // #dcdcaa
    p[(int)K::PreprocIdentifier]  = IM_COL32(156, 220, 254, 255);
    p[(int)K::Comment]            = IM_COL32(106, 153, 85, 255);  // #6a9955
    p[(int)K::MultiLineComment]   = IM_COL32(106, 153, 85, 255);
    p[(int)K::Background]         = IM_COL32(6, 6, 8, 255);       // #060608
    p[(int)K::Cursor]             = IM_COL32(174, 175, 173, 255);
    p[(int)K::Selection]          = IM_COL32(38, 79, 120, 140);
    p[(int)K::ErrorMarker]        = IM_COL32(255, 60, 60, 255);
    p[(int)K::Breakpoint]         = IM_COL32(220, 60, 60, 255);
    p[(int)K::LineNumber]         = IM_COL32(120, 120, 128, 255); // readable dim gutter
    p[(int)K::CurrentLineFill]    = IM_COL32(255, 255, 255, 12);
    p[(int)K::CurrentLineFillInactive] = IM_COL32(255, 255, 255, 6);
    p[(int)K::CurrentLineEdge]    = IM_COL32(255, 255, 255, 24);
    return p;
}

void module::core::graphics::ui::c_ui::add_tab(const std::string& name, const std::string& content)
{
    tab_counter++;
    std::string tab_name = name.empty() ? (("tab ") + std::to_string(tab_counter)) : name;
    editor_tabs.emplace_back(tab_name);
    editor_tabs.back().editor.SetPalette(mockup_editor_palette());
    if (!content.empty())
        editor_tabs.back().editor.SetText(content);
    active_tab = static_cast<int>(editor_tabs.size()) - 1;
}

void module::core::graphics::ui::c_ui::close_tab(int index)
{
    if (index < 0 || index >= (int)editor_tabs.size()) return;
    if (editor_tabs.size() <= 1) return;
    editor_tabs.erase(editor_tabs.begin() + index);
    if (active_tab >= (int)editor_tabs.size())
        active_tab = (int)editor_tabs.size() - 1;
}

void module::core::graphics::ui::c_ui::initialize()
{
    // Mockup opens on main.lua showing the beginning of InfiniteYield's source
    // (with line numbers and syntax colours), not the one-line loader.
    static const char* kMockupSource = R"LUA(if IY_LOADED and not _G.IY_DEBUG then
    -- error("Infinite Yield is already running!", 0)
    return
end

pcall(function() getgenv().IY_LOADED = true end)
if not game:IsLoaded() then game.Loaded:Wait() end

function missing(t, f, fallback)
    if type(f) == t then return f end
    return fallback
end

cloneref = missing("function", cloneref, function(...) return ... end)
gethui = missing("function", gethui or get_hidden_gui)
syn = missing("table", syn)
syn_protect_gui = missing("function", syn and syn.protect_gui)
sethidden = missing("function", sethiddenproperty or set_hidden_property)
gethidden = missing("function", gethiddenproperty or get_hidden_property)
queueteleport = missing("function", queue_on_teleport)
httprequest = missing("function", request or http_request)
everyClipboard = missing("function", setclipboard or toclipboard))LUA";
    add_tab(("main.lua"), kMockupSource);
}

static void execute_script(const std::string& src)
{
    if (src.empty()) return;
    task_scheduler::RequestExecution(src);
}

static void render_instruction(module::core::graphics::ui::ui_instruction_t* inst, const std::string& window_name)
{
    using namespace ImGui;
    switch (inst->type)
    {
    case module::core::graphics::ui::ui_instruction_t::TEXT:
    {
        if (!inst->text_title.empty() && !inst->text_content.empty())
            Text((("%s %s")), inst->text_title.c_str(), inst->text_content.c_str());
        else if (!inst->text_content.empty())
            Text((("%s")), inst->text_content.c_str());
        else
            Text((("%s")), inst->text_title.c_str());
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::BUTTON:
    {
        if (Button(inst->button_title.c_str()))
            module::core::graphics::ui::c_ui::fire_callback_void(inst->button_callback);
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::CHECKBOX:
    {
        std::string id = window_name + ("/") + inst->checkbox_title;
        bool& s = checkbox_states[id];
        if (Checkbox(inst->checkbox_title.c_str(), &s))
            module::core::graphics::ui::c_ui::fire_callback_bool(inst->checkbox_callback, s);
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::INPUT:
    {
        if (!inst->input_title.empty())
            Text((("%s")), inst->input_title.c_str());
        char buf[512];
        strncpy_s(buf, inst->input_text.c_str(), sizeof(buf) - 1);
        std::string label = (("##input") + std::to_string(reinterpret_cast<uintptr_t>(inst)));
        if (InputText(label.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            inst->input_text = buf;
            module::core::graphics::ui::c_ui::fire_callback_str(inst->input_callback, inst->input_text);
        }
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::SLIDER:
    {
        std::string id = window_name + ("/") + inst->slider_title;
        float& v = slider_states[id];
        if (SliderFloat(inst->slider_title.c_str(), &v, inst->slider_min, inst->slider_max))
            module::core::graphics::ui::c_ui::fire_callback_float(inst->slider_callback, v);
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::LISTBOX:
    {
        std::string id = window_name + ("/") + inst->listbox_title;
        if (!listbox_states.count(id)) listbox_states[id] = inst->current_item - 1;
        int& sel = listbox_states[id];
        if (BeginListBox(inst->listbox_title.c_str(), ImVec2(200, 100)))
        {
            for (int i = 0; i < (int)inst->items.size(); i++)
            {
                if (Selectable(inst->items[i].c_str(), sel == i))
                {
                    sel = i;
                    module::core::graphics::ui::c_ui::fire_callback_int(inst->listbox_callback, sel + 1);
                }
                if (sel == i) SetItemDefaultFocus();
            }
            EndListBox();
        }
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::COMBO:
    {
        std::string id = window_name + ("/") + inst->combo_title;
        if (!combo_states.count(id)) combo_states[id] = inst->current_item - 1;
        int& sel = combo_states[id];
        std::vector<const char*> items_c;
        for (auto& item : inst->items)
            items_c.push_back(item.c_str());
        if (!items_c.empty())
        {
            if (Combo(inst->combo_title.c_str(), &sel, items_c.data(), (int)items_c.size()))
                module::core::graphics::ui::c_ui::fire_callback_int(inst->combo_callback, sel + 1);
        }
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::COLOR_PICKER:
    {
        if (ColorEdit4(inst->text_title.c_str(), inst->color))
            module::core::graphics::ui::c_ui::fire_callback_color(inst->color_callback, inst->color[0], inst->color[1], inst->color[2], inst->color[3]);
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::PROGRESS_BAR:
    {
        std::string overlay = std::to_string((int)(inst->progress_value * 100)) + ("%");
        ProgressBar(inst->progress_value, ImVec2(-1, 0), overlay.c_str());
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::SEPARATOR: { Separator(); break; }
    case module::core::graphics::ui::ui_instruction_t::SAME_LINE: { SameLine(); break; }
    case module::core::graphics::ui::ui_instruction_t::SPACING: { Spacing(); break; }
    case module::core::graphics::ui::ui_instruction_t::TREE_NODE:
    {
        if (TreeNode(inst->treename_title.c_str()))
        {
            module::core::graphics::ui::c_ui::fire_callback_void(inst->treenode_callback);
            TreePop();
        }
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::TOOLTIP:
    {
        if (IsItemHovered())
        {
            BeginTooltip();
            Text(("%s"), inst->tooltip_text.c_str());
            EndTooltip();
        }
        break;
    }
    case module::core::graphics::ui::ui_instruction_t::COLUMNS: { Columns(inst->columns_count); break; }
    case module::core::graphics::ui::ui_instruction_t::NEXT_COLUMN: { NextColumn(); break; }
    default: break;
    }
}

void module::core::graphics::ui::c_ui::draw_lua_ui()
{
    using namespace ImGui;
    std::unordered_map<std::string, std::vector<module::core::graphics::ui::ui_instruction_t*>> window_map;
    for (auto& inst : ui_queue)
        window_map[inst.ui_title].push_back(&inst);
    for (auto& [window_name, instructions] : window_map)
    {
        if (!Begin(window_name.c_str())) { End(); continue; }
        bool has_tabbar = false;
        std::string tabbar_name;
        for (auto* inst : instructions)
        {
            if (inst->type == module::core::graphics::ui::ui_instruction_t::TABBAR)
            {
                has_tabbar = true;
                tabbar_name = inst->tabbar_title;
                break;
            }
        }
        if (has_tabbar && BeginTabBar(tabbar_name.c_str()))
        {
            std::unordered_map<std::string, std::vector<module::core::graphics::ui::ui_instruction_t*>> tab_content;
            for (auto* inst : instructions)
            {
                if (!inst->parent_tabbar.empty())
                    tab_content[inst->parent_tabbar].push_back(inst);
            }
            for (auto* inst : instructions)
            {
                if (inst->type == module::core::graphics::ui::ui_instruction_t::TABITEM && BeginTabItem(inst->tabitem_title.c_str()))
                {
                    auto it = tab_content.find(inst->tabitem_title);
                    if (it != tab_content.end())
                    {
                        for (auto* ci : it->second)
                            render_instruction(ci, window_name);
                    }
                    EndTabItem();
                }
            }
            EndTabBar();
        }
        for (auto* inst : instructions)
        {
            if (inst->parent_tabbar.empty() && inst->type != module::core::graphics::ui::ui_instruction_t::TABBAR)
                render_instruction(inst, window_name);
        }
        End();
    }
}

namespace
{
    const ImU32 kTextStrong = IM_COL32(232, 232, 234, 255);
    const ImU32 kTextMuted = IM_COL32(138, 139, 143, 255);
    const ImU32 kSurfacePressed = IM_COL32(255, 255, 255, 220);

    // Every icon is a cell of the Hugeicons atlas uploaded by the renderer.
    void draw_icon(int icon, ImVec2 pos, float size, ImU32 tint)
    {
        void* atlas = module::core::graphics::ui::c_ui::icon_atlas;
        if (!atlas)
            return;

        const float atlas_width = (float)module::core::graphics::ui::icon_atlas_width;
        const float u0 = (icon * module::core::graphics::ui::icon_atlas_cell) / atlas_width;
        const float u1 = ((icon + 1) * module::core::graphics::ui::icon_atlas_cell) / atlas_width;

        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)atlas, pos,
            ImVec2(pos.x + size, pos.y + size), ImVec2(u0, 0.0f), ImVec2(u1, 1.0f), tint);
    }

    // Navigation row: a hairline marker on the left when selected, nothing else.
    bool nav_row(const char* id, const char* label, int icon, bool active, float row_height)
    {
        const float scale = module::core::graphics::c_graphics::dpi_scale;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImVec2 pos = window->DC.CursorPos;
        float width = window->WorkRect.Max.x - window->DC.CursorPos.x;

        ImGui::InvisibleButton(id, ImVec2(width, row_height));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        if (active)
            window->DrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + row_height), IM_COL32(255, 255, 255, 14), 4.0f);
        else if (hovered)
            window->DrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + row_height), IM_COL32(255, 255, 255, 8), 4.0f);

        if (active)
            window->DrawList->AddRectFilled(ImVec2(pos.x, pos.y + 6.0f * scale), ImVec2(pos.x + 2.0f * scale, pos.y + row_height - 6.0f * scale),
                kSurfacePressed, 1.0f);

        const float icon_size = 17.0f * scale;
        const float pad = 14.0f * scale;
        draw_icon(icon, ImVec2(pos.x + pad, pos.y + (row_height - icon_size) * 0.5f), icon_size,
            active ? kTextStrong : IM_COL32(132, 133, 138, 255));

        ImVec2 text_size = ImGui::CalcTextSize(label);
        window->DrawList->AddText(ImVec2(pos.x + pad + icon_size + 11.0f * scale, pos.y + (row_height - text_size.y) * 0.5f),
            active ? kTextStrong : kTextMuted, label);
        return clicked;
    }

    // Flat button: white fill for the primary action, hairline outline otherwise.
    bool icon_button(const char* label, int icon, float width, float height, bool primary)
    {
        const float scale = module::core::graphics::c_graphics::dpi_scale;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.64f, 0.5f));
        if (primary)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.933f, 0.933f, 0.937f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.84f, 0.85f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.10f, 0.10f, 1.00f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 0.04f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 0.09f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 1.00f, 1.00f, 0.14f));
        }

        const bool clicked = ImGui::Button(label, ImVec2(width, height));

        if (!primary)
        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, 26), 4.0f);
        }

        ImGui::PopStyleColor(primary ? 4 : 3);
        ImGui::PopStyleVar();

        ImVec2 minimum = ImGui::GetItemRectMin();
        ImVec2 maximum = ImGui::GetItemRectMax();
        const float icon_size = 15.0f * scale;
        draw_icon(icon, ImVec2(minimum.x + 12.0f * scale, minimum.y + (maximum.y - minimum.y - icon_size) * 0.5f),
            icon_size, primary ? IM_COL32(18, 18, 18, 235) : IM_COL32(226, 226, 228, 235));
        return clicked;
    }
}

void module::core::graphics::ui::c_ui::draw()
{
    using namespace ImGui;

    static int active_nav = 1;           // 0: Home, 1: Editor, 2: Settings
    static int selected_script = 0;      // InfiniteYield
    static int active_dock_tab = 2;      // Terminal (like mockup)
    static bool dock_visible = true;
    static bool auto_scroll = true;

    ImGuiViewport* vp = GetMainViewport();
    float s = c_graphics::dpi_scale;
    s = ImMin(s, (vp->WorkSize.x - 8.0f) / 900.0f);
    s = ImMin(s, (vp->WorkSize.y - 8.0f) / 560.0f);

    const ImU32 bg_window       = IM_COL32(9, 9, 11, 255);
    const ImU32 bg_editor       = IM_COL32(6, 6, 8, 255);
    const ImU32 border_hairline = IM_COL32(255, 255, 255, 38);
    const ImU32 border_subtle   = IM_COL32(255, 255, 255, 20);
    const ImU32 text_pure       = IM_COL32(236, 236, 237, 255);
    const ImU32 text_sub        = IM_COL32(170, 170, 178, 255);
    const ImU32 text_dim        = IM_COL32(120, 120, 128, 255);
    const ImU32 text_green      = IM_COL32(48, 209, 88, 255);

    const float titlebar_h = 38.0f * s;
    const float sidebar_w  = 200.0f * s;
    const float tabrow_h   = 34.0f * s;
    const float toolbar_h  = 38.0f * s;
    const float dock_h     = dock_visible ? 120.0f * s : 28.0f * s;

    const ImVec2 card_size(900.0f * s, 560.0f * s);
    SetNextWindowSize(card_size, ImGuiCond_Always);
    static bool placed_once = false;
    if (!placed_once)
    {
        placed_once = true;
        SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - card_size.x) * 0.5f,
            vp->WorkPos.y + (vp->WorkSize.y - card_size.y) * 0.5f), ImGuiCond_Always);
    }

    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!Begin(("##paradox_root"), nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
    {
        End();
        PopStyleVar(2);
        return;
    }

    ImDrawList* dl = GetWindowDrawList();
    const ImVec2 o = GetWindowPos();
    const ImVec2 sz = GetWindowSize();
    s = ImMin(sz.x / 900.0f, sz.y / 560.0f);

    const float editor_x = sidebar_w;
    const float editor_w = sz.x - sidebar_w;
    const float body_y   = titlebar_h;
    const float tab_y    = body_y;
    const float toolbar_y = tab_y + tabrow_h;
    const float code_y   = toolbar_y + toolbar_h;
    const float dock_y   = sz.y - dock_h;
    const float code_h   = dock_y - code_y;

    auto P = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
    auto text = [&](float x, float y, float size, ImU32 c, const char* str) {
        dl->AddText(GetFont(), size * s, P(x, y), c, str);
    };
    auto text_bold = [&](float x, float y, float size, ImU32 c, const char* str) {
        dl->AddText(GetFont(), size * s, P(x, y), c, str);
        dl->AddText(GetFont(), size * s, P(x + 0.3f * s, y), c, str);
    };
    auto icon = [&](int id, float x, float y, float size, ImU32 c) {
        draw_icon(id, P(x, y), size * s, c);
    };

    // ── BASE WINDOW CARD BACKGROUND ───────────────────────────────────────
    dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), bg_window, 10.0f * s);

    // ── 1. TITLEBAR ───────────────────────────────────────────────────────
    dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + titlebar_h), bg_window, 10.0f * s, ImDrawFlags_RoundCornersTop);

    // Titlebar logo: Hugeicons Solid Parallelogram + Paradox
    const float logo_sz = 16.0f;
    const float logo_y = (titlebar_h / s - logo_sz) * 0.5f;
    icon(ICON_PARALLELOGRAM, 16.0f, logo_y, logo_sz, text_pure);
    text_bold(16.0f + logo_sz + 8.0f, (titlebar_h / s - 14.0f) * 0.5f, 14.5f, text_pure, "Paradox");

    // Center navigation icons
    const float nav_btn = 28.0f;
    const float nav_gap = 8.0f;
    const float nav_total = nav_btn * 3.0f + nav_gap * 2.0f;
    const float nav_x = (sz.x / s - nav_total) * 0.5f;
    const float nav_y = (titlebar_h / s - nav_btn) * 0.5f;

    const int nav_ids[3] = { ICON_PLAY, ICON_CODE, ICON_SETTINGS };
    for (int i = 0; i < 3; ++i)
    {
        const float x = nav_x + i * (nav_btn + nav_gap);
        SetCursorScreenPos(P(x, nav_y));
        if (InvisibleButton(("##nav_" + std::to_string(i)).c_str(), ImVec2(nav_btn * s, nav_btn * s)))
            active_nav = i;
        const ImU32 tint = (active_nav == i) ? text_pure : text_sub;
        icon(nav_ids[i], x + 6.0f, nav_y + 6.0f, 16.0f, tint);
    }

    // Drag region
    SetCursorScreenPos(o);
    InvisibleButton("##titlebar_drag", ImVec2(sz.x, titlebar_h));
    if (IsItemActive() && IsMouseDragging(ImGuiMouseButton_Left))
    {
        ImVec2 cur = GetWindowPos();
        SetWindowPos(ImVec2(cur.x + GetIO().MouseDelta.x, cur.y + GetIO().MouseDelta.y));
    }

    if (active_nav == 1)
    {
        // ── 2. SIDEBAR ────────────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(o.x, o.y + body_y), ImVec2(o.x + sidebar_w, o.y + sz.y), bg_window,
            10.0f * s, ImDrawFlags_RoundCornersBottomLeft);

        text_bold(16.0f, body_y / s + 14.0f, 14.0f, text_pure, "Scripts");

        const float row_h = 32.0f;
        const float row_w = sidebar_w / s - 20.0f;
        float row_y = body_y / s + 42.0f;
        for (int i = 0; i < IM_ARRAYSIZE(script_labels); ++i)
        {
            const float x = 10.0f;
            const ImVec2 a = P(x, row_y);
            const ImVec2 b = P(x + row_w, row_y + row_h);
            char id[32];
            snprintf(id, sizeof(id), "##script_%d", i);
            SetCursorScreenPos(a);
            InvisibleButton(id, ImVec2(row_w * s, row_h * s));
            const bool hovered = IsItemHovered();
            const bool sel = (selected_script == i);
            if (sel)
                dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14), 5.0f * s);
            else if (hovered)
                dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 8), 5.0f * s);

            icon(ICON_LUA, x + 8.0f, row_y + (row_h - 16.0f) * 0.5f, 16.0f,
                sel ? text_pure : text_sub);
            text(x + 32.0f, row_y + (row_h - 14.0f) * 0.5f, 13.5f,
                sel ? text_pure : text_sub, script_labels[i]);

            if (IsItemClicked(ImGuiMouseButton_Left))
            {
                selected_script = i;
                if (active_tab >= 0 && active_tab < (int)editor_tabs.size())
                    editor_tabs[active_tab].editor.SetText(script_sources[i]);
            }
            row_y += row_h + 2.0f;
        }

        // ── 3. TAB ROW ────────────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(o.x + editor_x, o.y + tab_y), ImVec2(o.x + sz.x, o.y + tab_y + tabrow_h), bg_window);

        if (active_tab >= 0 && active_tab < (int)editor_tabs.size())
        {
            const float tx = editor_x / s + 10.0f;
            const float ty = tab_y / s + 4.0f;
            const float th = 30.0f;
            const char* tab_name = editor_tabs[active_tab].name.c_str();
            const float tw = 34.0f + CalcTextSize(tab_name).x / s + 22.0f;

            dl->AddRectFilled(P(tx, ty), P(tx + tw, ty + th), bg_editor, 6.0f * s, ImDrawFlags_RoundCornersTop);
            dl->AddRect(P(tx, ty), P(tx + tw, ty + th), border_hairline, 6.0f * s, ImDrawFlags_RoundCornersTop);
            icon(ICON_CODE, tx + 10.0f, ty + (th - 16.0f) * 0.5f, 16.0f, text_sub);
            text(tx + 32.0f, ty + (th - 14.0f) * 0.5f, 13.5f, text_pure, tab_name);
            icon(ICON_CLOSE, tx + tw - 20.0f, ty + (th - 11.0f) * 0.5f, 11.0f,
                IM_COL32(200, 200, 200, 150));

            SetCursorScreenPos(P(tx + tw - 24.0f, ty));
            if (InvisibleButton("##tab_close", ImVec2(20.0f * s, th * s)) && editor_tabs.size() > 1)
                close_tab(active_tab);

            const float px = tx + tw + 4.0f;
            icon(ICON_PLUS, px + 7.0f, ty + 7.0f, 14.0f, text_sub);
            SetCursorScreenPos(P(px, ty));
            if (InvisibleButton("##tab_add", ImVec2(28.0f * s, 28.0f * s)))
                add_tab();
        }

        // ── 4. ACTION TOOLBAR ─────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(o.x + editor_x, o.y + toolbar_y), ImVec2(o.x + sz.x, o.y + toolbar_y + toolbar_h), bg_editor);

        struct ToolAction { const char* label; int icon_id; };
        const ToolAction actions[] = {
            { "Execute", ICON_PLAY }, { "Clear", ICON_DELETE },
            { "Open", ICON_CODE }, { "Copy", ICON_COPY },
        };
        float tool_x = editor_x / s + 16.0f;
        const float tool_y = toolbar_y / s + (toolbar_h / s - 16.0f) * 0.5f;
        for (int i = 0; i < IM_ARRAYSIZE(actions); ++i)
        {
            const ToolAction& a = actions[i];
            const float label_w = CalcTextSize(a.label).x / s;
            const float w = 16.0f + 6.0f + label_w;
            char tid[32];
            snprintf(tid, sizeof(tid), "##tool_%d", i);
            SetCursorScreenPos(P(tool_x, tool_y - 4.0f));
            const bool hovered = InvisibleButton(tid, ImVec2(w * s, 24.0f * s));
            const ImU32 tint = hovered ? text_pure : text_sub;
            icon(a.icon_id, tool_x, tool_y, 16.0f, tint);
            text(tool_x + 22.0f, tool_y, 13.5f, tint, a.label);
            if (IsItemClicked(ImGuiMouseButton_Left))
            {
                const std::string src = (active_tab >= 0 && active_tab < (int)editor_tabs.size())
                    ? editor_tabs[active_tab].editor.GetText() : std::string();
                if (i == 0) execute_script(src);
                else if (i == 1) { if (active_tab >= 0) editor_tabs[active_tab].editor.SetText(""); }
                else if (i == 3) SetClipboardText(src.c_str());
            }
            tool_x += w + 16.0f;
        }

        // ── 5. CODE CANVAS ────────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(o.x + editor_x, o.y + code_y), ImVec2(o.x + sz.x, o.y + dock_y), bg_editor);
        if (active_tab >= 0 && active_tab < (int)editor_tabs.size())
        {
            SetCursorScreenPos(ImVec2(o.x + editor_x, o.y + code_y));
            editor_tabs[active_tab].editor.Render("##code_editor",
                ImVec2(editor_w, code_h), false);
        }

        // ── 6. BOTTOM DOCK ────────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(o.x + editor_x, o.y + dock_y), ImVec2(o.x + sz.x, o.y + sz.y), bg_window,
            10.0f * s, ImDrawFlags_RoundCornersBottomRight);

        const float dock_header_h = 26.0f * s;
        SetCursorScreenPos(P(editor_x / s + 10.0f, dock_y / s + 2.0f));
        if (InvisibleButton("##dock_toggle", ImVec2(22.0f * s, 22.0f * s)))
            dock_visible = !dock_visible;
        icon(ICON_TERMINAL, editor_x / s + 10.0f, dock_y / s + 5.0f, 14.0f,
            dock_visible ? text_sub : text_pure);
        if (IsItemHovered())
            SetTooltip(dock_visible ? "Collapse panel" : "Expand panel");

        const char* dock_tabs[] = { "Problems", "Roblox Output", "Terminal" };
        float dock_x = editor_x / s + 36.0f;
        for (int i = 0; i < 3; ++i)
        {
            const char* label = dock_tabs[i];
            const float w = CalcTextSize(label).x / s;
            const bool active = (active_dock_tab == i);
            text(dock_x, dock_y / s + 5.0f, 12.5f, active ? text_pure : text_sub, label);
            if (active)
            {
                dl->AddRectFilled(ImVec2(o.x + dock_x * s, o.y + dock_y + dock_header_h - 2.0f),
                    ImVec2(o.x + (dock_x + w) * s, o.y + dock_y + dock_header_h), text_pure);
            }
            char did[32];
            snprintf(did, sizeof(did), "##dock_%d", i);
            SetCursorScreenPos(P(dock_x, dock_y / s + 2.0f));
            if (InvisibleButton(did, ImVec2(w * s, 22.0f * s)))
                active_dock_tab = i;
            dock_x += w + 18.0f;
        }

        // Dock log body
        if (dock_visible)
        {
            refresh_roblox_output();
            SetCursorScreenPos(ImVec2(o.x + editor_x + 16.0f * s, o.y + dock_y + dock_header_h + 6.0f * s));
            PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f * s));
            BeginChild("##dock_logs", ImVec2(editor_w - 32.0f * s, sz.y - (dock_y + dock_header_h + 10.0f * s)),
                false, ImGuiWindowFlags_None);
            {
                if (active_dock_tab == 2)
                {
                    PushStyleColor(ImGuiCol_Text, text_sub);
                    TextUnformatted("[Paradox Core] Runtime hooked into D3D11 Present (x64)");
                    TextUnformatted("[TaskScheduler] HybridJob found & execution pipeline ready");
                    PushStyleColor(ImGuiCol_Text, text_green);
                    TextUnformatted("[Client] Connected via local pipe");
                    PopStyleColor(2);

                    for (const auto& l : log_mgr.get_log_info())
                    {
                        if (!l.visible) continue;
                        const std::string cleaned = clean_module_log(l.content);
                        PushStyleColor(ImGuiCol_Text, l.color);
                        TextUnformatted(cleaned.c_str());
                        PopStyleColor();
                    }
                }
                else if (active_dock_tab == 0)
                {
                    int problem_count = 0;
                    for (const auto& l : g_roblox_lines)
                    {
                        if (!l.problem) continue;
                        problem_count++;
                        PushStyleColor(ImGuiCol_Text, IM_COL32(230, 180, 80, 240));
                        TextUnformatted(l.text.c_str());
                        PopStyleColor();
                    }
                    if (problem_count == 0)
                    {
                        PushStyleColor(ImGuiCol_Text, text_dim);
                        TextUnformatted("No problems detected in workspace.");
                        PopStyleColor();
                    }
                }
                else
                {
                    if (g_roblox_lines.empty())
                    {
                        PushStyleColor(ImGuiCol_Text, text_dim);
                        TextUnformatted("No console output recorded.");
                        PopStyleColor();
                    }
                    else
                    {
                        for (const auto& l : g_roblox_lines)
                        {
                            PushStyleColor(ImGuiCol_Text, l.color);
                            TextUnformatted(l.text.c_str());
                            PopStyleColor();
                        }
                    }
                }
                if (auto_scroll && GetScrollY() >= GetScrollMaxY() - 10.0f)
                    SetScrollHereY(1.0f);
            }
            EndChild();
            PopStyleVar();
        }
    }

    // ── ALTERNATE TABS: HOME / SETTINGS ───────────────────────────────────
    if (active_nav != 1)
    {
        const float content_y = body_y / s + 32.0f;
        if (active_nav == 0)
        {
            text_bold(28.0f, content_y, 24.0f, text_pure, "Paradox");
            text(28.0f, content_y + 34.0f, 13.5f, text_sub, "Pick a script to open it in the editor.");
            float y = content_y + 70.0f;
            for (int i = 0; i < IM_ARRAYSIZE(script_labels); ++i)
            {
                const float w = 320.0f;
                const ImVec2 a = P(28.0f, y);
                SetCursorScreenPos(a);
                if (InvisibleButton(("##home_script_" + std::to_string(i)).c_str(), ImVec2(w * s, 30.0f * s)))
                {
                    selected_script = i;
                    active_nav = 1;
                    if (active_tab >= 0 && active_tab < (int)editor_tabs.size())
                        editor_tabs[active_tab].editor.SetText(script_sources[i]);
                }
                if (IsItemHovered())
                    dl->AddRectFilled(a, P(28.0f + w, y + 30.0f), IM_COL32(255, 255, 255, 10), 4.0f * s);
                icon(ICON_LUA, 38.0f, y + 7.0f, 16.0f, IM_COL32(255, 255, 255, 255));
                text(64.0f, y + 8.0f, 13.5f, text_pure, script_labels[i]);
                y += 34.0f;
            }
        }
        else
        {
            text_bold(28.0f, content_y, 22.0f, text_pure, "Settings");
            text(28.0f, content_y + 36.0f, 13.5f, text_sub, "Hotkeys");
            text(28.0f, content_y + 60.0f, 13.5f, text_pure, "Toggle menu: INSERT / END / F2");
            text(28.0f, content_y + 86.0f, 13.5f, text_sub, "Module control");
            SetCursorScreenPos(P(28.0f, content_y + 114.0f));
            if (Button("Unload Process", ImVec2(150.0f * s, 30.0f * s)))
                exit(0);
        }
    }

    // ── CRISP STRUCTURAL HAIRLINE DIVIDERS (Guaranteed visible, drawn on top) ──
    // Titlebar bottom divider
    dl->AddRectFilled(ImVec2(o.x, o.y + titlebar_h), ImVec2(o.x + sz.x, o.y + titlebar_h + 1.0f), border_hairline);

    if (active_nav == 1)
    {
        // Sidebar right vertical divider
        dl->AddRectFilled(ImVec2(o.x + sidebar_w, o.y + titlebar_h), ImVec2(o.x + sidebar_w + 1.0f, o.y + sz.y), border_hairline);

        // Tab row bottom divider
        dl->AddRectFilled(ImVec2(o.x + sidebar_w, o.y + toolbar_y), ImVec2(o.x + sz.x, o.y + toolbar_y + 1.0f), border_hairline);

        // Action toolbar bottom divider
        dl->AddRectFilled(ImVec2(o.x + sidebar_w, o.y + code_y), ImVec2(o.x + sz.x, o.y + code_y + 1.0f), border_subtle);

        // Dock top divider
        dl->AddRectFilled(ImVec2(o.x + sidebar_w, o.y + dock_y), ImVec2(o.x + sz.x, o.y + dock_y + 1.0f), border_hairline);

        // Dock header bottom divider
        dl->AddRectFilled(ImVec2(o.x + sidebar_w, o.y + dock_y + 26.0f * s), ImVec2(o.x + sz.x, o.y + dock_y + 26.0f * s + 1.0f), border_subtle);
    }

    // Outer card perimeter rounded border
    dl->AddRect(o, ImVec2(o.x + sz.x, o.y + sz.y), border_hairline, 10.0f * s);

    End();
    PopStyleVar(2);
}

#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdlib>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <rtc/rtc.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "NetworkManager.h"
#include "minimap_reader.h"
#include "AudioManager.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ===========================================================================
// Estado compartilhado
// ===========================================================================
namespace {

std::atomic<bool> g_running(true);
std::atomic<bool> g_muted(false);
std::atomic<bool> g_deafened(false);
std::atomic<bool> g_pttActive(false);

// Posicao propria no minimapa (normalizada), publicada pela thread de visao.
std::atomic<bool>  g_selfPosValid(false);
std::atomic<float> g_selfX(0.0f);
std::atomic<float> g_selfY(0.0f);
std::atomic<int>   g_mapCandidates(0);
std::atomic<int>   g_pingMs(-1);        // -1 enquanto nao ha resposta

// Host do servidor, sem esquema nem porta ("ws://10.0.0.5:8080" -> "10.0.0.5").
std::string HostFromUrl(const std::string& url) {
    std::string host = url;
    const size_t scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return host;
}

// Ping ICMP de verdade (o mesmo que o jogo mostra), em milissegundos.
int PingHost(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* info = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &info) != 0 || !info) return -1;
    const IN_ADDR dest = ((sockaddr_in*)info->ai_addr)->sin_addr;
    freeaddrinfo(info);

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) return -1;

    char payload[32] = "lpvc";
    char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 16];
    const DWORD count = IcmpSendEcho(icmp, dest.S_un.S_addr, payload, sizeof(payload),
                                     nullptr, reply, sizeof(reply), 1000);
    int ms = -1;
    if (count > 0) {
        const ICMP_ECHO_REPLY* echo = (const ICMP_ECHO_REPLY*)reply;
        if (echo->Status == IP_SUCCESS) ms = (int)echo->RoundTripTime;
    }
    IcmpCloseHandle(icmp);
    return ms;
}

struct Settings {
    std::string name;
    std::string ip;
    int   role = 0;                 // 0 azul, 1 vermelho, 2 organizador
    float outputVolume = 100.0f;    // %
    float inputGain = 100.0f;       // %
    int   noiseSuppression = 2;     // 0 desligado, 1 baixa, 2 media, 3 alta
    bool  agc = true;
    bool  echoCancel = true;
    bool  vad = true;
    int   bitrate = 32000;
    std::string captureDevice;
    std::string playbackDevice;

    bool  proximityEnabled = true;
    bool  muteWithoutPosition = false;   // sem posicao -> silencio? (padrao nao)
    bool  organizersAlwaysAudible = true;
    float fullVolumeRange = 8.0f;        // % do mapa com volume total
    float maxRange = 25.0f;              // % do mapa onde zera

    MinimapRegion region;
    int   mapBrightness = 200;
    int   mapMinBox = 30;      // retangulo branco da camera no minimapa
    int   mapMaxBox = 250;

    int   muteKey = 'M';
    bool  pushToTalk = false;
    bool  loopback = false;
};

Settings g_settings;
std::mutex g_settingsMutex;   // protege os campos lidos pelas threads auxiliares

// Volume por participante, ajustado na UI.
std::unordered_map<uint32_t, float> g_peerVolume;
std::mutex g_peerVolumeMutex;

std::string ConfigPath() {
    char buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", buf, MAX_PATH) > 0) {
        std::string dir = std::string(buf) + "\\LPVC";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\config.ini";
    }
    return "lpvc_config.ini";
}

void SaveSettings(const Settings& s) {
    std::ofstream f(ConfigPath());
    if (!f) return;
    f << "name=" << s.name << "\n"
      << "ip=" << s.ip << "\n"
      << "role=" << s.role << "\n"
      << "outputVolume=" << s.outputVolume << "\n"
      << "inputGain=" << s.inputGain << "\n"
      << "noiseSuppression=" << s.noiseSuppression << "\n"
      << "agc=" << (int)s.agc << "\n"
      << "echoCancel=" << (int)s.echoCancel << "\n"
      << "vad=" << (int)s.vad << "\n"
      << "bitrate=" << s.bitrate << "\n"
      << "captureDevice=" << s.captureDevice << "\n"
      << "playbackDevice=" << s.playbackDevice << "\n"
      << "proximityEnabled=" << (int)s.proximityEnabled << "\n"
      << "muteWithoutPosition=" << (int)s.muteWithoutPosition << "\n"
      << "organizersAlwaysAudible=" << (int)s.organizersAlwaysAudible << "\n"
      << "fullVolumeRange=" << s.fullVolumeRange << "\n"
      << "maxRange=" << s.maxRange << "\n"
      << "regionX=" << s.region.x << "\n"
      << "regionY=" << s.region.y << "\n"
      << "regionW=" << s.region.w << "\n"
      << "regionH=" << s.region.h << "\n"
      << "mapBrightness=" << s.mapBrightness << "\n"
      << "mapMinBox=" << s.mapMinBox << "\n"
      << "mapMaxBox=" << s.mapMaxBox << "\n"
      << "muteKey=" << s.muteKey << "\n"
      << "pushToTalk=" << (int)s.pushToTalk << "\n"
      << "loopback=" << (int)s.loopback << "\n";
}

void LoadSettings(Settings& s) {
    std::ifstream f(ConfigPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        auto num = [&v]() { return atof(v.c_str()); };
        if      (k == "name") s.name = v;
        else if (k == "ip") s.ip = v;
        else if (k == "role") s.role = (int)num();
        else if (k == "outputVolume") s.outputVolume = (float)num();
        else if (k == "inputGain") s.inputGain = (float)num();
        else if (k == "noiseSuppression") s.noiseSuppression = (int)num();
        else if (k == "agc") s.agc = num() != 0;
        else if (k == "echoCancel") s.echoCancel = num() != 0;
        else if (k == "vad") s.vad = num() != 0;
        else if (k == "bitrate") s.bitrate = (int)num();
        else if (k == "captureDevice") s.captureDevice = v;
        else if (k == "playbackDevice") s.playbackDevice = v;
        else if (k == "proximityEnabled") s.proximityEnabled = num() != 0;
        else if (k == "muteWithoutPosition") s.muteWithoutPosition = num() != 0;
        else if (k == "organizersAlwaysAudible") s.organizersAlwaysAudible = num() != 0;
        else if (k == "fullVolumeRange") s.fullVolumeRange = (float)num();
        else if (k == "maxRange") s.maxRange = (float)num();
        else if (k == "regionX") s.region.x = (int)num();
        else if (k == "regionY") s.region.y = (int)num();
        else if (k == "regionW") s.region.w = (int)num();
        else if (k == "regionH") s.region.h = (int)num();
        else if (k == "mapBrightness") s.mapBrightness = (int)num();
        else if (k == "mapMinBox") s.mapMinBox = (int)num();
        else if (k == "mapMaxBox") s.mapMaxBox = (int)num();
        else if (k == "muteKey") s.muteKey = (int)num();
        else if (k == "pushToTalk") s.pushToTalk = num() != 0;
        else if (k == "loopback") s.loopback = num() != 0;
    }
}

uint32_t MakePeerId(const std::string& name) {
    uint32_t h = 2166136261u;                       // FNV-1a
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    std::random_device rd;
    h ^= (uint32_t)rd() * 2654435761u;              // evita colisao entre sessoes
    return h ? h : 1u;                              // 0 e reservado
}

// Atenuacao por distancia: volume total ate "full", queda suave ate "max".
float DistanceFalloff(float distPercent, float fullRange, float maxRange) {
    if (distPercent <= fullRange) return 1.0f;
    if (distPercent >= maxRange) return 0.0f;
    const float t = (distPercent - fullRange) / std::max(0.001f, maxRange - fullRange);
    const float s = 1.0f - t;
    return s * s * (3.0f - 2.0f * s);   // smoothstep: queda suave, sem degraus
}

bool LoadTextureFromFile(const char* filename, GLuint* outTexture, int* outW, int* outH) {
    int w = 0, h = 0;
    unsigned char* data = stbi_load(filename, &w, &h, nullptr, 4);
    if (!data) return false;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    *outTexture = tex; *outW = w; *outH = h;
    return true;
}

void ApplyHextechStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(20.0f, 18.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(14.0f, 12.0f);
    style.ItemInnerSpacing = ImVec2(10.0f, 8.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.01f, 0.05f, 0.07f, 0.85f);
    c[ImGuiCol_Border]          = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.03f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.06f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.88f, 0.73f, 0.40f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.95f, 1.00f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_TabActive]       = ImVec4(0.78f, 0.61f, 0.28f, 1.00f);
    c[ImGuiCol_TabUnfocused]    = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_PlotHistogram]   = ImVec4(0.35f, 0.85f, 0.45f, 1.00f);
    style.ScaleAllSizes(scale);
}

const ImVec4 kGold(0.78f, 0.61f, 0.28f, 1.0f);

std::string KeyName(int vk) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    char name[64] = {0};
    if (sc && GetKeyNameTextA((LONG)(sc << 16), name, sizeof(name)) > 0) return name;
    if (vk > 32 && vk < 127) return std::string(1, (char)vk);
    return "VK " + std::to_string(vk);
}

// Medidor de nivel com faixa verde/amarela/vermelha.
void LevelMeter(const char* id, float level, float width, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() * 0.55f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(18, 18, 18, 255), 4.0f);
    const float w = std::clamp(level, 0.0f, 1.0f) * width;
    ImU32 col = active ? IM_COL32(90, 215, 110, 255) : IM_COL32(90, 110, 130, 255);
    if (level > 0.85f) col = IM_COL32(225, 90, 70, 255);
    else if (level > 0.65f) col = IM_COL32(230, 195, 80, 255);
    if (w > 1.0f) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col, 4.0f);
    (void)id;
    ImGui::Dummy(ImVec2(width, h));
}

} // namespace

// ===========================================================================
int main() {
    SetProcessDPIAware();     // captura de tela e janela em pixels reais

    LoadSettings(g_settings);
    if (g_settings.region.w <= 0) g_settings.region = MinimapReader::AutoDetectRegion();
    // Configuracoes antigas guardavam volumes acima de 100%; a escala agora para ai.
    g_settings.outputVolume = std::clamp(g_settings.outputVolume, 0.0f, 100.0f);
    g_settings.inputGain = std::clamp(g_settings.inputGain, 10.0f, 100.0f);

    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(940, 780, "LPVC - Proximity Voice for League", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    HWND hwnd = glfwGetWin32Window(window);
    if (HICON icon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(101))) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float dpiScale = 1.0f;
    if (HDC screenDc = GetDC(nullptr)) {
        dpiScale = std::clamp(GetDeviceCaps(screenDc, LOGPIXELSX) / 96.0f, 1.0f, 3.0f);
        ReleaseDC(nullptr, screenDc);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyHextechStyle(dpiScale);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f * dpiScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    GLuint bgTexture = 0; int bgW = 0, bgH = 0;
    const bool hasBackground = LoadTextureFromFile("fundo.png", &bgTexture, &bgW, &bgH);

    // ---- estado local da UI ------------------------------------------------
    Settings ui = g_settings;
    char nameBuffer[64] = {0};
    char ipBuffer[128] = {0};
    strncpy(nameBuffer, ui.name.c_str(), sizeof(nameBuffer) - 1);
    strncpy(ipBuffer, ui.ip.c_str(), sizeof(ipBuffer) - 1);

    std::vector<AudioDeviceInfo> captureDevices = AudioManager::ListDevices(true);
    std::vector<AudioDeviceInfo> playbackDevices = AudioManager::ListDevices(false);
    int captureIndex = -1, playbackIndex = -1;
    for (size_t i = 0; i < captureDevices.size(); ++i)
        if (captureDevices[i].name == ui.captureDevice) captureIndex = (int)i;
    for (size_t i = 0; i < playbackDevices.size(); ++i)
        if (playbackDevices[i].name == ui.playbackDevice) playbackIndex = (int)i;

    bool isConnected = false;
    bool isBindingKey = false;
    bool showAdvancedMap = false;
    uint32_t localId = 0;
    std::string statusMessage;

    NetworkManager net;
    AudioManager audio;
    std::thread infoThread, minimapThread, keyThread, pingThread;

    GLuint previewTexture = 0;
    std::vector<uint8_t> previewPixels;
    int previewW = 0, previewH = 0;
    std::mutex previewMutex;
    std::atomic<bool> previewRequested(false);
    std::atomic<bool> previewDirty(false);

    // =======================================================================
    while (!glfwWindowShouldClose(window)) {
        const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        const bool minimized = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;

        // Em segundo plano (durante a partida) a interface quase nao consome CPU.
        if (minimized) {
            glfwWaitEventsTimeout(0.5);
            continue;
        }
        if (focused) glfwPollEvents();
        else         glfwWaitEventsTimeout(0.1);

        int w, h;
        glfwGetWindowSize(window, &w, &h);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("Painel", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBackground);

        // -------------------------------------------------------------- LOGIN
        if (!isConnected) {
            const float centerX = (float)w * 0.5f;
            ImGui::SetCursorPosY((float)h * 0.12f);

            const char* titleBig = "LPVC";
            ImGui::PushFont(nullptr, 50.0f * dpiScale);
            ImGui::SetCursorPosX(centerX - ImGui::CalcTextSize(titleBig).x * 0.5f);
            ImGui::TextColored(kGold, "%s", titleBig);
            ImGui::PopFont();

            const char* titleSmall = "Proximity Voice Chat for League";
            ImGui::SetCursorPosX(centerX - ImGui::CalcTextSize(titleSmall).x * 0.5f);
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.0f), "%s", titleSmall);

            ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();

            const float fieldW = 340.0f * dpiScale;
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::Text("Nome de Invocador:");
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::PushItemWidth(fieldW);
            ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer));
            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::Text("Sua Funcao:");
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::RadioButton("Equipe Azul", &ui.role, 0); ImGui::SameLine();
            ImGui::RadioButton("Equipe Vermelha", &ui.role, 1); ImGui::SameLine();
            ImGui::RadioButton("Organizador", &ui.role, 2);

            ImGui::Spacing();
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::Text("Endereco IP do servidor (Radmin):");
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::PushItemWidth(fieldW);
            ImGui::InputText("##ip", ipBuffer, sizeof(ipBuffer));
            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::Text("Microfone:");
            ImGui::SetCursorPosX(centerX - fieldW * 0.5f);
            ImGui::PushItemWidth(fieldW);
            {
                const char* label = (captureIndex >= 0 && captureIndex < (int)captureDevices.size())
                                        ? captureDevices[captureIndex].name.c_str() : "Padrao do Windows";
                if (ImGui::BeginCombo("##cap", label)) {
                    if (ImGui::Selectable("Padrao do Windows", captureIndex < 0)) captureIndex = -1;
                    for (int i = 0; i < (int)captureDevices.size(); ++i)
                        if (ImGui::Selectable(captureDevices[i].name.c_str(), captureIndex == i)) captureIndex = i;
                    ImGui::EndCombo();
                }
            }
            ImGui::PopItemWidth();

            ImGui::Spacing(); ImGui::Spacing();
            if (!statusMessage.empty()) {
                ImGui::SetCursorPosX(centerX - ImGui::CalcTextSize(statusMessage.c_str()).x * 0.5f);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", statusMessage.c_str());
            }

            ImGui::SetCursorPosX(centerX - 80.0f * dpiScale);
            const bool canConnect = (strlen(nameBuffer) > 0);
            if (!canConnect) ImGui::BeginDisabled();
            if (ImGui::Button("CONECTAR", ImVec2(160 * dpiScale, 46 * dpiScale))) {
                ui.name = nameBuffer;
                ui.ip = ipBuffer;
                if (captureIndex >= 0) ui.captureDevice = captureDevices[captureIndex].name; else ui.captureDevice.clear();
                if (playbackIndex >= 0) ui.playbackDevice = playbackDevices[playbackIndex].name; else ui.playbackDevice.clear();

                std::string url = ui.ip.empty() ? std::string("127.0.0.1") : ui.ip;
                if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0)
                    url = "ws://" + url + ":8080";

                localId = MakePeerId(ui.name);

                const ma_device_id* capId = (captureIndex >= 0) ? &captureDevices[captureIndex].id : nullptr;
                const ma_device_id* playId = (playbackIndex >= 0) ? &playbackDevices[playbackIndex].id : nullptr;

                if (!audio.Init(capId, playId)) {
                    statusMessage = audio.LastError();
                } else {
                    statusMessage.clear();
                    audio.SetOutputGain(ui.outputVolume / 100.0f);
                    audio.SetInputGain(ui.inputGain / 100.0f);
                    audio.SetNoiseSuppression(ui.noiseSuppression);
                    audio.SetAgc(ui.agc);
                    audio.SetEchoCancel(ui.echoCancel);
                    audio.SetVad(ui.vad);
                    audio.SetBitrate(ui.bitrate);
                    audio.SetLoopback(ui.loopback);

                    {
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                    }
                    SaveSettings(ui);

                    rtc::InitLogger(rtc::LogLevel::Error);
                    net.Init(url, localId);

                    audio.SetNetworkSendCallback([&net](const std::vector<uint8_t>& packet) {
                        net.SendAudio(packet);
                    });
                    net.SetAudioReceivedCallback([&audio](uint32_t peer, uint32_t seq,
                                                          const uint8_t* data, size_t len) {
                        audio.PushPacket(peer, seq, data, len);
                    });
                    net.SetPeerLeftCallback([&audio](uint32_t peer) { audio.RemovePeer(peer); });

                    // -- heartbeat + limpeza de quem saiu --------------------
                    infoThread = std::thread([&net]() {
                        while (g_running.load()) {
                            std::string name; int role;
                            {
                                std::lock_guard<std::mutex> lock(g_settingsMutex);
                                name = g_settings.name; role = g_settings.role;
                            }
                            net.SendInfo(name, role);
                            net.PruneStale(12000);
                            for (int i = 0; i < 20 && g_running.load(); ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    });

                    // -- visao do minimapa + volume por distancia ------------
                    minimapThread = std::thread([&net, &audio, &previewMutex, &previewPixels,
                                                 &previewW, &previewH, &previewRequested, &previewDirty]() {
                        MinimapReader minimap;
                        MinimapRegion appliedRegion{};
                        while (g_running.load()) {
                            Settings s;
                            {
                                std::lock_guard<std::mutex> lock(g_settingsMutex);
                                s = g_settings;
                            }
                            if (s.region.x != appliedRegion.x || s.region.y != appliedRegion.y ||
                                s.region.w != appliedRegion.w || s.region.h != appliedRegion.h) {
                                minimap.Configure(s.region);
                                appliedRegion = s.region;
                            }
                            minimap.SetBrightness(s.mapBrightness);
                            minimap.SetBoxSize(s.mapMinBox, s.mapMaxBox);

                            // Sem proximidade e sem previa aberta, nao ha por que
                            // capturar a tela: economiza CPU durante a partida.
                            const bool needsVision = s.proximityEnabled || previewRequested.load();
                            MapPos self;
                            if (needsVision && minimap.Capture()) {
                                self = minimap.FindSelf();
                                g_mapCandidates.store(minimap.GetCandidateCount());
                                if (previewRequested.load()) {
                                    std::lock_guard<std::mutex> lock(previewMutex);
                                    if (minimap.GetPreviewRGBA(previewPixels, previewW, previewH))
                                        previewDirty.store(true);
                                }
                            }

                            g_selfPosValid.store(self.valid());
                            if (self.valid()) {
                                g_selfX.store(self.x);
                                g_selfY.store(self.y);
                                net.SendPosition(self.x, self.y);
                            }

                            // Ganho final de cada participante = slider x distancia.
                            for (const PeerInfo& peer : net.GetPeers()) {
                                float userGain = 1.0f;
                                {
                                    std::lock_guard<std::mutex> lock(g_peerVolumeMutex);
                                    auto it = g_peerVolume.find(peer.id);
                                    if (it != g_peerVolume.end()) userGain = it->second;
                                    else g_peerVolume[peer.id] = 1.0f;
                                }

                                float fade = 1.0f;
                                if (s.proximityEnabled &&
                                    !(s.organizersAlwaysAudible && peer.role == 2)) {
                                    if (self.valid() && peer.hasPosition) {
                                        const float dx = (peer.x - self.x) * 100.0f;
                                        const float dy = (peer.y - self.y) * 100.0f;
                                        const float dist = std::sqrt(dx * dx + dy * dy);
                                        fade = DistanceFalloff(dist, s.fullVolumeRange, s.maxRange);
                                    } else {
                                        // Sem leitura de posicao nao silenciamos por padrao:
                                        // e melhor ouvir todo mundo do que ficar mudo.
                                        fade = s.muteWithoutPosition ? 0.0f : 1.0f;
                                    }
                                }
                                audio.SetPeerGain(peer.id, userGain * fade);
                            }
                            audio.SetPeerGain(AudioManager::kLoopbackPeerId, 1.0f);

                            std::this_thread::sleep_for(std::chrono::milliseconds(80));
                        }
                    });

                    // -- ping do servidor ------------------------------------
                    pingThread = std::thread([&net, url]() {
                        WSADATA wsa;
                        WSAStartup(MAKEWORD(2, 2), &wsa);
                        const std::string host = HostFromUrl(url);
                        while (g_running.load()) {
                            net.SendPing();
                            for (int i = 0; i < 10 && g_running.load(); ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                            // O ping do proprio WebSocket e o que vale (mede o
                            // caminho da voz). Se o servidor for antigo e nao
                            // responder, cai no ping ICMP tradicional.
                            const int wsPing = net.GetPingMs();
                            g_pingMs.store(wsPing >= 0 ? wsPing : PingHost(host));

                            for (int i = 0; i < 10 && g_running.load(); ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        WSACleanup();
                    });

                    // -- teclas globais (mute / push-to-talk) ----------------
                    keyThread = std::thread([&audio]() {
                        bool wasPressed = false;
                        while (g_running.load()) {
                            int key; bool ptt;
                            {
                                std::lock_guard<std::mutex> lock(g_settingsMutex);
                                key = g_settings.muteKey; ptt = g_settings.pushToTalk;
                            }
                            if (key > 0) {
                                const bool pressed = (GetAsyncKeyState(key) & 0x8000) != 0;
                                if (ptt) {
                                    g_pttActive.store(pressed);
                                    audio.SetMuted(!pressed);
                                } else {
                                    if (pressed && !wasPressed) {
                                        const bool m = !g_muted.load();
                                        g_muted.store(m);
                                        audio.SetMuted(m);
                                    }
                                }
                                wasPressed = pressed;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    });

                    isConnected = true;
                }
            }
            if (!canConnect) ImGui::EndDisabled();
        }
        // ---------------------------------------------------------- CONECTADO
        else {
            ImGui::TextColored(kGold, "Invocador: %s", ui.name.c_str());
            ImGui::SameLine();
            if (net.IsConnected())
                ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f), "   [ conectado ]");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "   [ reconectando... ]");

            ImGui::Spacing();

            if (ImGui::BeginTabBar("Tabs", ImGuiTabBarFlags_NoTooltip)) {
                // ---------------------------------------------------- STATUS
                if (ImGui::BeginTabItem("Status")) {
                    ImGui::Spacing();

                    bool m = g_muted.load();
                    if (ImGui::Checkbox("Silenciar Microfone", &m)) { g_muted.store(m); audio.SetMuted(m); }
                    ImGui::SameLine(300 * dpiScale);
                    bool d = g_deafened.load();
                    if (ImGui::Checkbox("Mutar Tudo (Deafen)", &d)) { g_deafened.store(d); audio.SetDeafened(d); }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    bool noiseOn = (ui.noiseSuppression > 0);
                    if (ImGui::Checkbox("Supressao de ruido", &noiseOn)) {
                        ui.noiseSuppression = noiseOn ? AudioManager::kNoiseMedium : AudioManager::kNoiseOff;
                        audio.SetNoiseSuppression(ui.noiseSuppression);
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                    }
                    ImGui::TextDisabled("Powered by RNNoise");

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    const int ping = g_pingMs.load();
                    ImGui::Text("Ping:");
                    ImGui::SameLine();
                    if (ping < 0) {
                        ImGui::TextDisabled("sem resposta");
                    } else {
                        const ImVec4 cor = (ping < 60)  ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f)
                                         : (ping < 120) ? ImVec4(0.95f, 0.8f, 0.35f, 1.0f)
                                                        : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
                        ImGui::TextColored(cor, "%d ms", ping);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (g_selfPosValid.load()) {
                        ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                            "Minimapa: posicao localizada (%.0f%%, %.0f%%)",
                            g_selfX.load() * 100.0f, g_selfY.load() * 100.0f);
                    } else if (ui.proximityEnabled) {
                        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f),
                            "Minimapa: sem leitura - calibre na aba Minimapa%s",
                            ui.muteWithoutPosition ? " (todos estao mudos!)" : " (ouvindo todos)");
                    } else {
                        ImGui::TextDisabled("Proximidade desligada - todos com volume total.");
                    }

                    ImGui::EndTabItem();
                }

                // ------------------------------------------------------ TIME
                if (ImGui::BeginTabItem("Time")) {
                    ImGui::Spacing();
                    std::vector<PeerInfo> peers = net.GetPeers();

                    PeerInfo me;
                    me.id = localId; me.name = ui.name; me.role = ui.role;
                    peers.push_back(me);

                    const float availWidth = ImGui::GetContentRegionAvail().x;
                    const float childWidth = (availWidth * 0.5f) - 10.0f;

                    auto drawGroup = [&](const char* title, int role, ImVec4 bg, ImVec4 rowBg,
                                         float width, float height, int maxCount) {
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
                        ImGui::PushStyleColor(ImGuiCol_Border, kGold);
                        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
                        ImGui::BeginChild(title, ImVec2(width, height), true, ImGuiWindowFlags_NoScrollbar);

                        const float innerWidth = ImGui::GetWindowSize().x;
                        ImGui::SetCursorPosX((innerWidth - ImGui::CalcTextSize(title).x) * 0.5f);
                        ImGui::TextColored(kGold, "%s", title);
                        ImGui::Separator();

                        int count = 0;
                        for (PeerInfo& peer : peers) {
                            if (peer.role != role || count >= maxCount) continue;
                            const bool isMe = (peer.id == localId);

                            ImGui::PushStyleColor(ImGuiCol_ChildBg, rowBg);
                            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                            const std::string rowId = std::string(title) + "_" + std::to_string(peer.id);
                            if (ImGui::BeginChild(rowId.c_str(), ImVec2(0, 46 * dpiScale), true,
                                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                                const float rowH = 46 * dpiScale;
                                ImGui::SetCursorPosY((rowH - ImGui::GetFontSize()) * 0.5f);
                                ImGui::SetCursorPosX(12.0f);

                                ImGui::Text("%s%s", peer.name.c_str(), isMe ? " (voce)" : "");

                                if (!isMe) {
                                    float vol = 1.0f;
                                    {
                                        std::lock_guard<std::mutex> lock(g_peerVolumeMutex);
                                        auto it = g_peerVolume.find(peer.id);
                                        vol = (it != g_peerVolume.end()) ? it->second : 1.0f;
                                    }
                                    ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f * dpiScale);
                                    ImGui::SetCursorPosY((rowH - ImGui::GetFrameHeight()) * 0.5f);
                                    ImGui::PushItemWidth(100.0f * dpiScale);
                                    if (ImGui::SliderFloat(("##v" + rowId).c_str(), &vol, 0.0f, 2.0f, "%.1fx")) {
                                        std::lock_guard<std::mutex> lock(g_peerVolumeMutex);
                                        g_peerVolume[peer.id] = vol;
                                    }
                                    ImGui::PopItemWidth();
                                }
                            }
                            ImGui::EndChild();
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor(1);
                            count++;
                        }
                        if (count == 0) ImGui::TextDisabled("  (vazio)");

                        ImGui::EndChild();
                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor(2);
                    };

                    drawGroup("EQUIPE AZUL", 0, ImVec4(0.10f, 0.18f, 0.30f, 0.85f),
                              ImVec4(0.15f, 0.25f, 0.45f, 0.90f), childWidth, 300 * dpiScale, 5);
                    ImGui::SameLine(0.0f, 20.0f);
                    drawGroup("EQUIPE VERMELHA", 1, ImVec4(0.35f, 0.12f, 0.12f, 0.85f),
                              ImVec4(0.45f, 0.15f, 0.15f, 0.90f), childWidth, 300 * dpiScale, 5);

                    ImGui::Spacing();
                    const float orgWidth = std::min(460.0f * dpiScale, availWidth);
                    ImGui::SetCursorPosX((availWidth - orgWidth) * 0.5f + ImGui::GetCursorStartPos().x);
                    drawGroup("ORGANIZADORES", 2, ImVec4(0.06f, 0.06f, 0.06f, 0.90f),
                              ImVec4(0.15f, 0.15f, 0.15f, 0.95f), orgWidth, 200 * dpiScale, 3);

                    ImGui::EndTabItem();
                }

                // ----------------------------------------------------- AUDIO
                if (ImGui::BeginTabItem("Audio")) {
                    ImGui::Spacing();
                    bool dirty = false;
                    ImGui::PushItemWidth(320 * dpiScale);

                    ImGui::Text("Volume de saida");
                    if (ImGui::SliderFloat("##outvol", &ui.outputVolume, 0.0f, 100.0f, "%.0f%%")) {
                        audio.SetOutputGain(ui.outputVolume / 100.0f);
                        dirty = true;
                    }

                    ImGui::Text("Ganho do microfone");
                    if (ImGui::SliderFloat("##ingain", &ui.inputGain, 10.0f, 100.0f, "%.0f%%")) {
                        audio.SetInputGain(ui.inputGain / 100.0f);
                        dirty = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("AGC: %d%%", audio.GetAgcGainPercent());

                    ImGui::Text("Entrada");
                    ImGui::SameLine(160 * dpiScale);
                    LevelMeter("mic2", audio.GetInputLevel(), 320 * dpiScale, audio.IsTransmitting());

                    ImGui::PopItemWidth();
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Checkbox("Normalizar volume da voz (AGC)", &ui.agc)) { audio.SetAgc(ui.agc); dirty = true; }
                    ImGui::SameLine(340 * dpiScale);
                    if (ImGui::Checkbox("Cancelar eco", &ui.echoCancel)) { audio.SetEchoCancel(ui.echoCancel); dirty = true; }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::Text("Dispositivos");

                    ImGui::PushItemWidth(320 * dpiScale);
                    const char* capLabel = (captureIndex >= 0 && captureIndex < (int)captureDevices.size())
                                               ? captureDevices[captureIndex].name.c_str() : "Padrao do Windows";
                    int newCapture = captureIndex, newPlayback = playbackIndex;
                    if (ImGui::BeginCombo("Microfone", capLabel)) {
                        if (ImGui::Selectable("Padrao do Windows", captureIndex < 0)) newCapture = -1;
                        for (int i = 0; i < (int)captureDevices.size(); ++i)
                            if (ImGui::Selectable(captureDevices[i].name.c_str(), captureIndex == i)) newCapture = i;
                        ImGui::EndCombo();
                    }
                    const char* playLabel = (playbackIndex >= 0 && playbackIndex < (int)playbackDevices.size())
                                                ? playbackDevices[playbackIndex].name.c_str() : "Padrao do Windows";
                    if (ImGui::BeginCombo("Saida", playLabel)) {
                        if (ImGui::Selectable("Padrao do Windows", playbackIndex < 0)) newPlayback = -1;
                        for (int i = 0; i < (int)playbackDevices.size(); ++i)
                            if (ImGui::Selectable(playbackDevices[i].name.c_str(), playbackIndex == i)) newPlayback = i;
                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();

                    if (newCapture != captureIndex || newPlayback != playbackIndex) {
                        captureIndex = newCapture;
                        playbackIndex = newPlayback;
                        ui.captureDevice = (captureIndex >= 0) ? captureDevices[captureIndex].name : "";
                        ui.playbackDevice = (playbackIndex >= 0) ? playbackDevices[playbackIndex].name : "";
                        const ma_device_id* capId = (captureIndex >= 0) ? &captureDevices[captureIndex].id : nullptr;
                        const ma_device_id* playId = (playbackIndex >= 0) ? &playbackDevices[playbackIndex].id : nullptr;
                        if (!audio.Restart(capId, playId)) statusMessage = audio.LastError();
                        dirty = true;
                    }
                    if (ImGui::Button("Atualizar lista de dispositivos")) {
                        captureDevices = AudioManager::ListDevices(true);
                        playbackDevices = AudioManager::ListDevices(false);
                        captureIndex = playbackIndex = -1;
                        for (size_t i = 0; i < captureDevices.size(); ++i)
                            if (captureDevices[i].name == ui.captureDevice) captureIndex = (int)i;
                        for (size_t i = 0; i < playbackDevices.size(); ++i)
                            if (playbackDevices[i].name == ui.playbackDevice) playbackIndex = (int)i;
                    }

                    ImGui::Spacing();
                    if (ImGui::Checkbox("Ouvir a propria voz", &ui.loopback)) {
                        audio.SetLoopback(ui.loopback);
                        dirty = true;
                    }

                    if (dirty) {
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                    }
                    ImGui::EndTabItem();
                }

                // -------------------------------------------------- MINIMAPA
                if (ImGui::BeginTabItem("Minimapa")) {
                    previewRequested.store(showAdvancedMap);
                    ImGui::Spacing();
                    bool dirty = false;

                    if (ImGui::Checkbox("Voz por proximidade", &ui.proximityEnabled)) dirty = true;
                    ImGui::SameLine(340 * dpiScale);
                    if (ImGui::Checkbox("Organizadores sempre audiveis", &ui.organizersAlwaysAudible)) dirty = true;
                    if (ImGui::Checkbox("Silenciar quando nao houver leitura do mapa", &ui.muteWithoutPosition)) dirty = true;

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("Detectar automaticamente", ImVec2(240 * dpiScale, 38 * dpiScale))) {
                        ui.region = MinimapReader::AutoDetectRegion();
                        dirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(showAdvancedMap ? "Avancado  v" : "Avancado  >",
                                      ImVec2(160 * dpiScale, 38 * dpiScale))) {
                        showAdvancedMap = !showAdvancedMap;
                    }

                    if (!showAdvancedMap) {
                        ImGui::Spacing();
                        ImGui::TextDisabled(g_selfPosValid.load()
                            ? "Minimapa localizado. Nada a ajustar."
                            : "Sem leitura do minimapa: abra Avancado para calibrar.");
                    }

                    if (showAdvancedMap) {
                        ImGui::Spacing();
                        ImGui::Text("Regiao capturada da tela");

                        int screenW, screenH;
                        MinimapReader::GetScreenSize(screenW, screenH);
                        ImGui::PushItemWidth(300 * dpiScale);
                        if (ImGui::SliderInt("X", &ui.region.x, 0, std::max(0, screenW - 64))) dirty = true;
                        if (ImGui::SliderInt("Y", &ui.region.y, 0, std::max(0, screenH - 64))) dirty = true;
                        if (ImGui::SliderInt("Largura", &ui.region.w, 64, screenW)) dirty = true;
                        if (ImGui::SliderInt("Altura", &ui.region.h, 64, screenH)) dirty = true;
                        if (ImGui::SliderInt("Brilho minimo (branco)", &ui.mapBrightness, 120, 255)) dirty = true;
                        if (ImGui::SliderInt("Tamanho min. da camera", &ui.mapMinBox, 10, 150)) dirty = true;
                        if (ImGui::SliderInt("Tamanho max. da camera", &ui.mapMaxBox, 40, 400)) dirty = true;
                        if (ui.mapMaxBox <= ui.mapMinBox) ui.mapMaxBox = ui.mapMinBox + 10;
                        ImGui::PopItemWidth();

                        ImGui::Spacing();
                        ImGui::TextDisabled("O que e rastreado e o retangulo branco da camera no minimapa.");
                        ImGui::Text("Previa (%d candidatos)  %s", g_mapCandidates.load(),
                                    g_selfPosValid.load() ? "- posicao localizada" : "- procurando...");
                    }
                    if (showAdvancedMap) {
                        std::lock_guard<std::mutex> lock(previewMutex);
                        if (previewDirty.exchange(false) && previewW > 0 && previewH > 0) {
                            if (!previewTexture) glGenTextures(1, &previewTexture);
                            glBindTexture(GL_TEXTURE_2D, previewTexture);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, previewW, previewH, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, previewPixels.data());
                        }
                    }
                    if (showAdvancedMap && previewTexture) {
                        const float side = 230.0f * dpiScale;
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        ImGui::Image((ImTextureID)(intptr_t)previewTexture, ImVec2(side, side));
                        if (g_selfPosValid.load()) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 p(origin.x + g_selfX.load() * side, origin.y + g_selfY.load() * side);
                            dl->AddCircle(p, 10.0f, IM_COL32(80, 255, 120, 255), 0, 2.5f);
                        }
                    }

                    if (dirty) {
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                    }
                    ImGui::EndTabItem();
                } else {
                    previewRequested.store(false);
                }

                // ---------------------------------------------- CONFIGURACOES
                if (ImGui::BeginTabItem("Configuracoes")) {
                    ImGui::Spacing();
                    bool dirty = false;

                    if (ImGui::Checkbox("Push-to-talk (fala so enquanto segura a tecla)", &ui.pushToTalk)) {
                        dirty = true;
                        if (!ui.pushToTalk) { g_muted.store(false); audio.SetMuted(false); }
                    }
                    ImGui::Spacing();

                    if (isBindingKey) {
                        ImGui::Button("Pressione a nova tecla...", ImVec2(260 * dpiScale, 40 * dpiScale));
                        for (int k = 8; k <= 255; k++) {
                            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_ESCAPE) continue;
                            if (GetAsyncKeyState(k) & 0x8000) {
                                ui.muteKey = k;
                                isBindingKey = false;
                                dirty = true;
                                break;
                            }
                        }
                        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) isBindingKey = false;
                    } else {
                        const std::string label = (ui.pushToTalk ? "Tecla de fala: " : "Tecla de mute: ") + KeyName(ui.muteKey);
                        if (ImGui::Button(label.c_str(), ImVec2(260 * dpiScale, 40 * dpiScale))) isBindingKey = true;
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Servidor: %s", net.Url().c_str());
                    ImGui::TextDisabled("Seu ID: %u", localId);

                    ImGui::Spacing();
                    if (ImGui::Button("Salvar configuracoes", ImVec2(220 * dpiScale, 38 * dpiScale))) {
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                        SaveSettings(ui);
                    }

                    if (dirty) {
                        std::lock_guard<std::mutex> lock(g_settingsMutex);
                        g_settings = ui;
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        ImGui::End();
        ImGui::Render();

        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.01f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (hasBackground) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, bgTexture);
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
            glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
            glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
            glEnd();
            glDisable(GL_TEXTURE_2D);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ---- encerramento ordenado --------------------------------------------
    g_running.store(false);
    if (isConnected) {
        net.SendBye();
        SaveSettings(ui);
    }
    if (infoThread.joinable())    infoThread.join();
    if (minimapThread.joinable()) minimapThread.join();
    if (keyThread.joinable())     keyThread.join();
    if (pingThread.joinable())    pingThread.join();

    audio.Shutdown();
    net.Close();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

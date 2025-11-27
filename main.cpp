#define SDL_MAIN_USE_CALLBACKS

#include <stdio.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* tex;

namespace ImGui {
    bool SliderDouble(const char* label, double* v, double v_min, double v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
        return SliderScalar(label, ImGuiDataType_Double, v, &v_min, &v_max, format, flags);
    }
}

constexpr int w = 1080;
constexpr int h = 720;

struct Params {
    // The atmosphere scale height.
    double scale_height;
    // The exterior bound of the atmosphere.
    double atmos_bound;
    // The camera options.
    double cx, cy, zoom;

    Params() {
        scale_height = 7994. / 6.3781e6;
        atmos_bound = 1.25;
        cx = 0.0;
        cy = 0.0;
        zoom = 12.0;
    }
};

static bool dirty = true;
static Params params;

static void compute(Params& p, double x, double y, uint8_t& r, uint8_t& g, uint8_t& b) {
    double bound_h_sq = p.atmos_bound * p.atmos_bound - x * x - y * y;
    
    if (bound_h_sq < 0) {
        r = g = b = 0;
        return;
    }
    
    double planet_h_sq = 1 - x * x - y * y;
    
    bool hit = planet_h_sq >= 0;

    double zi = sqrt(bound_h_sq);
    double zf = hit ? sqrt(planet_h_sq) : -zi;

    r = g = b = (zi - zf) / 2 / p.atmos_bound * 255;
}

ImVec4 clear_color = ImVec4(0, 0, 0, 1);

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    window = SDL_CreateWindow("Renderer", (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (window == nullptr) {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        SDL_Log("Error: SDL_CreateRenderer(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    io.Fonts->AddFontFromFileTTF("./assets/fonts/Cousine-Regular.ttf");

    tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(window))
        return SDL_APP_SUCCESS;

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    ImGuiIO& io = ImGui::GetIO();

    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
    {
        SDL_Delay(10);
        return SDL_APP_CONTINUE;
    }

    uint8_t* pixels = nullptr;
    int pitch = -1;

    SDL_LockTexture(tex, NULL, (void**)&pixels, &pitch);

    if (dirty) {
        double pixel_zoom = std::min(w, h) / exp(params.zoom);

        for (int i = 0; i < w; i++) {
            for (int j = 0; j < h; j++) {
                double x = params.cx + pixel_zoom * (i - w / 2);
                double y = params.cy + pixel_zoom * (j - h / 2);
                
                size_t off = (i + j * w) * 3;

                uint8_t& r = pixels[off++];
                uint8_t& g = pixels[off++];
                uint8_t& b = pixels[off];

                compute(params, x, y, r, g, b);
            }
        }

        dirty = false;
    }

    SDL_UnlockTexture(tex);

    SDL_Rect view;
    SDL_GetRenderViewport(renderer, &view);
    
    double scale = std::min((double) view.h / h, (double) view.w / w);

    SDL_FRect texdst;
    
    texdst.w = w * scale;
    texdst.h = h * scale;

    texdst.x = (view.w - texdst.w) / 2.;
    texdst.y = (view.h - texdst.h) / 2.;

    SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    SDL_RenderClear(renderer);

    SDL_RenderTexture(renderer, tex, NULL, &texdst);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Preview");

    ImGui::Text("window=%dx%d", view.w, view.h);
    ImGui::Text("viewport=(%.2f, %.2f), %.2fx%.2f", texdst.x, texdst.y, texdst.w, texdst.h);

    ImGui::ColorEdit3("Background", (float*) &clear_color);

    ImGui::End();
    
    ImGui::Begin("Parameters");
    
    dirty |= ImGui::SliderDouble("Scale height", &params.scale_height, 0.0, 0.01, "%.6f");
    dirty |= ImGui::SliderDouble("Atmosphere bound", &params.atmos_bound, 1.0, 1.5);
    dirty |= ImGui::SliderDouble("Camera X", &params.cx, -2.0, 2.0);
    dirty |= ImGui::SliderDouble("Camera Y", &params.cy, -2.0, 2.0);
    dirty |= ImGui::SliderDouble("Zoom", &params.zoom, 0.0, 20.0);

    ImGui::End();

    ImGui::Render();

    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_SetRenderScale(renderer, 1, 1);

    SDL_RenderPresent(renderer);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

#define SDL_MAIN_USE_CALLBACKS

#include <stdio.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <optional>

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

struct Vec3 {
    double x, y, z;

    Vec3() {
        x = y = z = 0;
    }

    Vec3(double x, double y, double z) {
        this->x = x;
        this->y = y;
        this->z = z;
    }

    auto operator+(const Vec3& other) const {
        return Vec3 { this->x + other.x, this->y + other.y, this->z + other.z };
    }
    
    auto operator*(const Vec3& other) const {
        return this->x * other.x + this->y * other.y + this->z * other.z;
    }
    
    auto operator*(double scalar) const {
        return Vec3 { this->x * scalar, this->y * scalar, this->z * scalar };
    }
};

struct Params {
    // The atmosphere scale height.
    double scale_height;
    // The exterior bound of the atmosphere.
    double atmos_bound;
    // The step size of the rays.
    double view_step, sun_step;
    // The sun angle
    double sun_azimuth, sun_altitude;
    // The camera options.
    double cx, cy, zoom;

    Params() {
        scale_height = 7994. / 6.3781e6;
        atmos_bound = 1.01;

        view_step = 0.01;
        sun_step = 0.1;

        sun_azimuth = 270;
        sun_altitude = 45;

        cx = 0.0;
        cy = 0.0;
        zoom = 12.0;
    }
};

struct Derived {
    // The normalized sun vector.
    Vec3 sun;
    // The sun step vector.
    Vec3 sun_step;
    // The squared bound.
    double b_sq;

    Derived(Params& p) {
        double azi = p.sun_azimuth * M_PI / 180;
        double alt = p.sun_altitude * M_PI / 180;

        sun.x = cos(alt) * cos(azi);
        sun.y = cos(alt) * sin(azi);
        sun.z = sin(alt);

        sun_step = sun * p.sun_step;

        b_sq = p.atmos_bound * p.atmos_bound;
    }
};

struct integrator {
    double acc = 0;
    std::optional<double> prev;

    void add(double val, double step) {
        if (prev)
            acc += (*prev + val) / 2 * step;

        prev = val;
    }
};

static bool dirty = true;
static Params params;

static void compute(Params& p, Derived& d, double x, double y, uint8_t& r, uint8_t& g, uint8_t& b) {
    double bound_h_sq = d.b_sq - x * x - y * y;
    
    if (bound_h_sq < 0) {
        r = g = b = 0;
        return;
    }
    
    double planet_h_sq = 1 - x * x - y * y;
    
    bool hit = planet_h_sq >= 0;

    double z = sqrt(bound_h_sq);
    double zf = hit ? sqrt(planet_h_sq) : -z;

    integrator density;
    integrator intensity;

    do {
        // Step along the view ray
        double view_step = p.view_step;
        double zn = z - view_step;

        if (zn <= zf) {
            zn = zf;
            view_step = z - zn;
        }

        z = zn;

        Vec3 v(x, y, z);

        // Evaluate density
        double vv = v*v;
        double view_h = sqrt(vv) - 1;
        double view_rho = exp(-view_h / params.scale_height);

        density.add(view_rho, view_step);

        double sv = d.sun * v;

        if (sv < 0 && vv - sv*sv < 1) {
            intensity.add(0, view_step);
            continue;
        }

        integrator in;

        Vec3 w = v;
        double in_r;

        do {
            w = w + d.sun_step;

            double ww = w*w;
            in_r = sqrt(ww);
            double in_h = in_r - 1;
            double in_rho = exp(-in_h / params.scale_height);

            density.add(in_rho, p.sun_step);
        } while (in_r < p.atmos_bound);

        intensity.add(view_rho * exp(-in.acc - density.acc), view_step);
    } while (z != zf);

    double scaled = intensity.acc * 3000;

    if (scaled > 255) {
        r = 255;
        g = b = 0;
    } else {
        r = g = b = scaled;
    }
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

        Derived d(params);

        for (int i = 0; i < w; i++) {
            for (int j = 0; j < h; j++) {
                double x = params.cx + pixel_zoom * (i - w / 2);
                double y = params.cy + pixel_zoom * (j - h / 2);
                
                size_t off = (i + j * w) * 3;

                uint8_t& r = pixels[off++];
                uint8_t& g = pixels[off++];
                uint8_t& b = pixels[off];

                compute(params, d, x, y, r, g, b);
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
    dirty |= ImGui::SliderDouble("View step", &params.view_step, 1e-3, 1.0);
    dirty |= ImGui::SliderDouble("Sun step", &params.sun_step, 1e-3, 1.0);
    dirty |= ImGui::SliderDouble("Sun azimuth", &params.sun_azimuth, 0, 360.0);
    dirty |= ImGui::SliderDouble("Sun altitude", &params.sun_altitude, -90.0, 90.0);
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

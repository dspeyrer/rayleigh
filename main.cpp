#define SDL_MAIN_USE_CALLBACKS

#include <stdio.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <limits>
#include <cmath>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* tex;

static SDL_Surface* diffuse;
static SDL_Surface* illuminants;

    bool SliderDouble(const char* label, double* v, double v_min, double v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, v, &v_min, &v_max, format, flags);
    }

bool SliderExp(const char* label, double* v, double v_min, double v_max) {
    int e = std::floor(std::log10(*v));
    double m = *v / pow(10, e);
    
    bool changed = false;
    
    ImGui::PushID(label);
    ImGui::PushItemWidth(76);
    changed |= SliderDouble("##", &m, 1.0, 10.0);
    ImGui::SameLine();
    changed |= ImGui::SliderInt(label, &e, v_min, v_max);
    ImGui::PopItemWidth();
    ImGui::PopID();

    if (changed && m < 10.)
        *v = m * pow(10, e);

    return changed;
}

constexpr int w = 500;
constexpr int h = 500;

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

    Vec3 operator+(const Vec3& other) const {
        return { x + other.x, y + other.y, z + other.z };
    }
    
    double operator*(const Vec3& other) const {
        return x * other.x + y * other.y + z * other.z;
    }
    
    Vec3 operator*(double scalar) const {
        return { x * scalar, y * scalar, z * scalar };
    }
};

struct Mat3 {
    Vec3 rows[3];

    Mat3() {}

    Mat3(Vec3 r0, Vec3 r1, Vec3 r2) {
        rows[0] = r0;
        rows[1] = r1;
        rows[2] = r2;
    }

    Mat3 transpose() const {
        return Mat3(
            { rows[0].x, rows[1].x, rows[2].x },
            { rows[0].y, rows[1].y, rows[2].y },
            { rows[0].z, rows[1].z, rows[2].z }
        );
    }

    static Mat3 RotX(double theta) {
        double s = sin(theta);
        double c = cos(theta);

        return Mat3(
            { 1, 0, 0 },
            { 0, c,-s },
            { 0, s, c }
        );
    }

    static Mat3 RotY(double theta) {
        double s = sin(theta);
        double c = cos(theta);

        return Mat3(
            { c, 0, s },
            { 0, 1, 0 },
            {-s, 0, c }
        );
    }

    static Mat3 RotZ(double theta) {
        double s = sin(theta);
        double c = cos(theta);

        return Mat3(
            { c,-s, 0 },
            { s, c, 0 },
            { 0, 0, 1 }
        );
    }

    Vec3 operator*(const Vec3& v) const {
        return Vec3(
            v * rows[0],
            v * rows[1],
            v * rows[2]
        );
    }

    Mat3 operator*(const Mat3& other) const {
        Mat3 t = other.transpose();

        return Mat3(
            { rows[0] * t.rows[0], rows[0] * t.rows[1], rows[0] * t.rows[2] }, 
            { rows[1] * t.rows[0], rows[1] * t.rows[1], rows[1] * t.rows[2] }, 
            { rows[2] * t.rows[0], rows[2] * t.rows[1], rows[2] * t.rows[2] }
        );
    }
};

struct Params {
    // The atmosphere scale height.
    double scale_height = 7994. / 6.3781e6;
    // The exterior bound of the atmosphere.
    double atmos_bound = 1.01;
    // The step size of the rays.
    double view_step = 1000, sun_step = 1000, step_scale = 0;
    // The angle to the sun.
    double sun_azimuth = 270, sun_altitude = 0;
    // The solar irradiance of the colors of each channel.
    float ssi[3] = { 0.75, 0.85, 1.0 };
    // The color of the surface illuminants.
    float surf_illum[3] = { 1.0, 1.0, 1.0 };

    int sky_quant = 100;
    double sky_eval_step = .05;

    // The atmospheric parameter.
    double k = 128139406146;

    double exposure = 2.0;


    // The orientation of the planet.
    double planet_yaw = 0, planet_pitch = 0, planet_roll = 0;

    // The camera options.
    double cx = 0, cy = 0, zoom = -2;
};

struct sky_table {
    std::vector<double> values;
    sky_table(size_t samples);
};

struct Derived {
    // The normalized sun vector.
    Vec3 sun;
    // The squared bound.
    double b_sq;
    // The transform matrix of the planet texture.
    Mat3 planet_transform;

    // Lookup tables for sky illuminances.
    sky_table sky_r;
    sky_table sky_g;
    sky_table sky_b;

    Derived(Params& p) : sky_r(p.sky_quant), sky_g(p.sky_quant), sky_b(p.sky_quant) {
        double azi = p.sun_azimuth * M_PI / 180;
        double alt = p.sun_altitude * M_PI / 180;

        sun.x = cos(alt) * cos(azi);
        sun.y = cos(alt) * sin(azi);
        sun.z = sin(alt);

        planet_transform = Mat3::RotY(p.planet_yaw) * Mat3::RotX(p.planet_pitch) * Mat3::RotZ(p.planet_roll);

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

static size_t prev_head;

static std::mutex m;
static std::condition_variable cv;
static bool dirty = true;
static Params params;

static std::atomic<size_t> head;
static uint8_t buffer[h][w][3];

static double sample_nearest(SDL_Surface* surf, int x, int y, int c, bool wrap) {
    if (wrap) {
        x %= surf->w;
        y %= surf->h;
    }

    if (x < 0 || x >= surf->w || y < 0 || y >= surf->h) {
        printf("out-of-bounds texture sample (%d, %d) for texture of size (%d, %d)\n", x, y, surf->w, surf->h);
        return -1;
    }

    if (surf->format == SDL_PIXELFORMAT_RGB24)
        return *((uint8_t*)surf->pixels + y * surf->pitch + x * 3 + c) / 255.;
    
    if (surf->format == SDL_PIXELFORMAT_INDEX8)
        return *((uint8_t*)surf->pixels + y * surf->pitch + x) / 255.;

    printf("unsupported pixel format %x\n", surf->format);
    return -1;
}

static double sample_bilinear(SDL_Surface* surf, double x, double y, int c, bool wrap) {
    int l = (int) x;
    int u = (int) y;

    int r = l + 1;
    int d = u + 1;

    double tx = fmod(x, 1);
    double ty = fmod(y, 1);

    double lu = sample_nearest(surf, l, u, c, wrap);
    double ru = sample_nearest(surf, r, u, c, wrap);
    double ld = sample_nearest(surf, l, d, c, wrap);
    double rd = sample_nearest(surf, r, d, c, wrap);

    double vu = lu + tx * (ru - lu);
    double vd = ld + tx * (rd - ld);
    return vd = vu + ty * (vd - vu);
}

static double sample_texture(Params& p, Derived& d, SDL_Surface* surf, int c, Vec3 v) {
    v = d.planet_transform * v;

    double sx = (atan2(v.x, v.z) / M_PI / 2 + 0.5) * surf->w;
    double sy = (asin(v.y) / M_PI + 0.5) * surf->h;

    return sample_bilinear(surf, sx, sy, c, true);
}

constexpr double illum_y_min = (-75.0 / 90 + 1) / 2;
constexpr double illum_y_max = ( 65.0 / 90 + 1) / 2;

static double sample_illuminants(Params& p, Derived& d, int c, Vec3 v) {
    v = d.planet_transform * v;

    double sx = (atan2(v.x, v.z) / M_PI / 2 + 0.5) * illuminants->w;
    double sy = (asin(v.y) / M_PI + 0.5);

    if (sy < illum_y_min || sy > illum_y_max) return 0.0;

    sy -= illum_y_min;
    sy /= (illum_y_max - illum_y_min);

    sy *= illuminants->h;

    return sample_bilinear(illuminants, sx, sy, c, true);
}

static double optical_depth_to_sun(Params& p, Derived& d, double z, double r) {
    if (z < 0 && r < 1)
        return std::numeric_limits<double>::infinity();

    integrator depth;
    double step_size = 0;

    while (true) {
        double planet_distance = hypot(r, z);
        double h = planet_distance - 1;
        double rho = exp(-h / params.scale_height);
        
        depth.add(rho, step_size);

        if (planet_distance >= p.atmos_bound) break;

        step_size = exp(p.step_scale * h) / p.sun_step;
        z += step_size;
    }

    return depth.acc;
}

// Accumulates in-scattered light starting at point `v` on the surface along ray `r`, where the sun is in the +Z direction.
static double eval_from_surface(Params& p, Derived& d, double k, int c, Vec3 v, Vec3 r) {
    integrator depth;
    integrator intensity;

    double step_size = 0;

    while (true) {
        double v_v = v*v;
        double planet_distance = sqrt(v_v);
        double h = planet_distance  - 1;
        double rho = exp(-h / params.scale_height);

        depth.add(rho, step_size);

        double proj = v.z;
        double orth = hypot(v.x, v.y);

        double depth_to_sun = optical_depth_to_sun(p, d, proj, orth);
        double scatter_in = rho * exp(-4 * M_PI * k * (depth_to_sun + depth.acc));

        intensity.add(scatter_in, step_size);   

        if (planet_distance >= p.atmos_bound)
            return intensity.acc * p.ssi[c] * k;

        step_size = exp(p.step_scale * h) / p.view_step;
        v = v + r * step_size;
    }    
}

static double sky_light_at(Params& p, Derived& d, double k, int c, double cos_a) {
    double sin_a = sqrt(1 - cos_a*cos_a);

    double acc = 0;
    int n = 0;

    // Select a point Q on the sphere.
    Vec3 q(sin_a, 0, cos_a);

    // Get the basis vectors for the hemisphere around Q.
    Vec3 bx(-cos_a, 0, sin_a);
    Vec3 by(0, 1, 0);
    Vec3 bz(q); 

    double y = p.sky_eval_step / 2;

    while (y <= 1) {
        double x = p.sky_eval_step / 2;

        while (x*x + y*y <= 1) {
            do {
                Vec3 r = bx * x + by * y + bz * sqrt(1 - x*x - y*y);

                acc += eval_from_surface(p, d, k, c, q, r);
                n++;

                x = -x;
            } while (x < 0);

            x += p.sky_eval_step;
        }

        y += p.sky_eval_step;
    }

    acc /= n;
    return acc;
}

sky_table::sky_table(size_t samples) : values(samples, NAN) {};

static double sky_get_or_compute(sky_table& table, Params& p, Derived& d, double k, int c, int cos_a_quant) {
    if (cos_a_quant < 0)
        cos_a_quant = 0;
    if (cos_a_quant >= table.values.size())
        cos_a_quant = table.values.size() - 1;
    
    if (isnan(table.values[cos_a_quant])) {
        double cos_a = cos_a_quant * 2.0 / (table.values.size() - 1) - 1;
        cos_a = cos_a*cos_a*cos_a;
        table.values[cos_a_quant] = sky_light_at(p, d, k, c, cos_a);
    }

    return table.values[cos_a_quant];
}

static double sky_eval(sky_table& table, Params& p, Derived& d, double k, int c, double cos_a) {
    double t = (table.values.size() - 1) * (1 + cbrt(cos_a)) / 2;

    int tl = t;
    int tr = tl + 1;

    double tt = fmod(t, 1);

    double vl = sky_get_or_compute(table, p, d, k, c, tl);
    double vr = sky_get_or_compute(table, p, d, k, c, tr);

    return vl + tt * (vr - vl);
}

static double compute(Params& p, Derived& d, double k, double x, double y, int c) {
    double bound_h_sq = d.b_sq - x * x - y * y;

    if (bound_h_sq < 0) return 0.;

    double planet_h_sq = 1 - x * x - y * y;

    bool hit = planet_h_sq >= 0;

    double z = sqrt(bound_h_sq);
    double zf = hit ? sqrt(planet_h_sq) : -z;

    integrator depth;
    integrator intensity;

    double step_size = 0;

    while (true) {
        Vec3 v(x, y, z);

        double v_v = v*v;
        double h = sqrt(v_v) - 1;
        double rho = exp(-h / params.scale_height);

        depth.add(rho, step_size);

        double proj = v * d.sun;
        double orth = sqrt(v_v - proj*proj);

        double depth_to_sun = optical_depth_to_sun(p, d, proj, orth);
        double scatter_in = rho * exp(-4 * M_PI * k * (depth_to_sun + depth.acc));

        intensity.add(scatter_in, step_size);   

        if (z == zf) {
            if (hit) {
                double direct = scatter_in * std::max(proj, 0.);
                double sky;

                if (c == 0) sky = sky_eval(d.sky_r, p, d, k, c, proj);
                else if (c == 1) sky = sky_eval(d.sky_g, p, d, k, c, proj);
                else if (c == 2) sky = sky_eval(d.sky_b, p, d, k, c, proj);
                else assert(false);

                intensity.acc += (direct + sky) * sample_texture(p, d, diffuse, c, v);
            }

            intensity.acc *= p.ssi[c] * k;

            if (hit)
                intensity.acc += p.surf_illum[c] * exp(-4 * M_PI * k * depth.acc) * sample_illuminants(p, d, c, v);

            return intensity.acc;
        };

        step_size = exp(p.step_scale * h) / p.view_step;
        double zn = z - step_size;

        if (zn <= zf) {
            zn = zf;
            step_size = z - zn;
        }

        z = zn;
    }
}

void worker() {    
    while (true) {
        std::unique_lock lk(m);
        cv.wait(lk, []{ return dirty; });

        Params p = params;

        dirty = false;
        head.store(0);

        lk.unlock();

        Derived d(p);

        double kr = p.k * pow(760, -4);
        double kg = p.k * pow(555, -4);
        double kb = p.k * pow(495, -4);

        double pixel_zoom = exp(-p.zoom) / std::min(w, h);

        size_t off = 0;

        while (true) {
            int i = off / w;
            int j = off % w;

            double y = p.cy + pixel_zoom * (i - h / 2);
            double x = p.cx + pixel_zoom * (j - w / 2);

            buffer[i][j][0] = 255 * (1. - exp(-p.exposure * compute(p, d, kr, x, y, 0)));
            buffer[i][j][1] = 255 * (1. - exp(-p.exposure * compute(p, d, kg, x, y, 1)));
            buffer[i][j][2] = 255 * (1. - exp(-p.exposure * compute(p, d, kb, x, y, 2)));

            size_t prev = head.exchange(++off, std::memory_order_release);
            
            if (off >= w * h) break;
            if (prev == SIZE_MAX) break;
        }
    }
}

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

    io.Fonts->AddFontDefault();

    tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);

    printf("Loading assets...\n");

    diffuse = IMG_Load("./assets/images/NASA_BlueMarble_2004_11.png");

    if (!diffuse) {
        SDL_Log("Error: IMG_Load(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    illuminants = IMG_Load("./assets/images/EOS_VNL_v2.png");

    if (!illuminants) {
        SDL_Log("Error: IMG_Load(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    printf("Loading complete.\n");

    std::thread w(worker);
    w.detach();

    cv.notify_one();

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

    size_t cur_head = head.load(std::memory_order_acquire);

    if (cur_head > prev_head && cur_head != SIZE_MAX) {
        uint8_t* pixels = nullptr;
        int pitch = -1;

        SDL_LockTexture(tex, NULL, (void**)&pixels, &pitch);

        for (int off = prev_head; off < cur_head; off++) {
            int i = off / w;
            int j = off % w;

            for (int c = 0; c < 3; c++)
                pixels[i*pitch + j*3 + c] = buffer[i][j][c];
        }

        SDL_UnlockTexture(tex);

        prev_head = cur_head;
    }

    SDL_Rect view;
    SDL_GetRenderViewport(renderer, &view);

    double scale = std::min((double) view.h / h, (double) view.w / w);

    SDL_FRect texdst;

    texdst.w = w * scale;
    texdst.h = h * scale;

    texdst.x = (view.w - texdst.w) / 2.;
    texdst.y = (view.h - texdst.h) / 2.;

    SDL_SetRenderDrawColorFloat(renderer, 0.0, 0.0, 0.0, 1.0);
    SDL_RenderClear(renderer);

    SDL_RenderTexture(renderer, tex, NULL, &texdst);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Parameters");
    ImGui::PushItemWidth(160);

    std::unique_lock lk(m);

    ImGui::SeparatorText("Atmosphere");

    dirty |= SliderDouble("Scale height", &params.scale_height, 0.0, 0.01, "%.6f");
    dirty |= SliderDouble("Bound", &params.atmos_bound, 1.0, 1.5);
    dirty |= SliderDouble("View step", &params.view_step, 10, 1e6);
    dirty |= SliderDouble("Sun step", &params.sun_step, 10, 1e6);
    dirty |= SliderDouble("Step scale", &params.step_scale, 0.0, 2000.0);
    dirty |= SliderExp("K", &params.k, 0.0, 15.0);
    dirty |= SliderDouble("Exposure", &params.exposure, 0.0, 10.0);

    ImGui::SeparatorText("Sun");

    dirty |= SliderDouble("Azimuth", &params.sun_azimuth, 0, 360.0);
    dirty |= SliderDouble("Altitude", &params.sun_altitude, -90.0, 90.0);
    dirty |= ImGui::ColorEdit3("SSI", params.ssi);
    
    ImGui::SeparatorText("Surface");
    
    dirty |= SliderDouble("Planet yaw", &params.planet_yaw, -M_PI, M_PI);
    dirty |= SliderDouble("Planet pitch", &params.planet_pitch, -M_PI, M_PI);
    dirty |= SliderDouble("Planet roll", &params.planet_roll, -M_PI, M_PI);
    dirty |= ImGui::ColorEdit3("Surface illuminants", params.surf_illum);
    
    ImGui::SeparatorText("Sky");

    dirty |= ImGui::SliderInt("Lookup quantization", &params.sky_quant, 0, 1000);
    dirty |= SliderDouble("Evaluation density", &params.sky_eval_step, 0., 1.);

    ImGui::SeparatorText("Camera");
    
    dirty |= SliderDouble("Camera X", &params.cx, -2.0, 2.0);
    dirty |= SliderDouble("Camera Y", &params.cy, -2.0, 2.0);
    dirty |= SliderDouble("Zoom", &params.zoom, -5.0, 5.0);

    if (dirty) {
        cv.notify_one();
        head.store(SIZE_MAX);
        prev_head = 0;
    }
    
    lk.unlock();

    ImGui::SeparatorText("Status");

    ImGui::ProgressBar((float) cur_head / (w * h));

    ImGui::PopItemWidth();
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

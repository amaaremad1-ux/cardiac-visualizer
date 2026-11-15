// gl_smoke_test.cpp - exercises the advanced OpenGL 3.3 renderer off the UI:
// creates a hidden core-profile context, initializes TrajectoryRenderer, feeds
// a synthetic multi-segment delay-embedding, renders many frames, and asserts
// that (a) no GL error is ever raised and (b) a valid output texture is
// produced, while reporting the measured frame rate.
//
// If no 3.3 core context can be created (headless CI / no GPU) the test SKIPS
// with exit code 0 rather than failing: the desktop app already degrades to a
// DrawList fallback in that case, and we must not require a GPU to validate the
// core library.
#include "cardiac/types.hpp"
#include "trajectory_renderer.hpp"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// cgl.hpp is included AFTER glfw3.h and no ImGui header is present, satisfying
// the render-TU isolation rule (it remaps gl* to the loader's pointers).
#include "cgl.hpp"

namespace {
int failures = 0;
int checks = 0;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        ++checks;                                                     \
        if (!(cond)) {                                                \
            ++failures;                                               \
            std::fprintf(stderr, "FAIL %s [%d]\n", msg, __LINE__);     \
        }                                                             \
    } while (0)

// A synthetic delay-embedding-like curve split into two segments (a gap), so
// the renderer must never bridge them and must emit a red break marker.
cardiac::VisualizationSnapshot make_snapshot() {
    cardiac::VisualizationSnapshot snap;
    snap.state = cardiac::ApplicationState::ANALYSIS_READY;
    snap.mode = cardiac::SessionMode::SyntheticTest;
    snap.revision = 7;
    std::uint64_t idx = 0;
    const auto add_run = [&](std::uint64_t seg, int count, double phase0) {
        for (int i = 0; i < count; ++i) {
            const double t = phase0 + i * 0.05;
            cardiac::TrajectoryPoint p;
            p.index = idx++;
            p.segment_id = seg;
            p.position.x = 0.8 * std::sin(t);
            p.position.y = 0.6 * std::sin(t * 1.3 + 0.7);
            p.position.z = 0.5 * std::cos(t * 0.9);
            snap.points.push_back(p);
        }
    };
    add_run(0, 220, 0.0);
    add_run(1, 180, 5.0);  // second segment after a gap
    return snap;
}
} // namespace

int main() {
    if (!glfwInit()) {
        std::printf("GLFW init unavailable - GL smoke test SKIPPED\n");
        return 0;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "gl-smoke", nullptr, nullptr);
    if (!win) {
        std::printf("No OpenGL 3.3 core context (headless/no GPU) - GL smoke test SKIPPED\n");
        glfwTerminate();
        return 0;
    }
    glfwMakeContextCurrent(win);

    cardiac::ui::TrajectoryRenderer renderer;
    if (!renderer.init()) {
        std::fprintf(stderr, "Renderer init failed on a live 3.3 context: %s\n",
                     renderer.error().c_str());
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    CHECK(renderer.ready(), "renderer ready after init");
    CHECK(glGetError() == GL_NO_ERROR, "no GL error after init");

    const cardiac::VisualizationSnapshot snap = make_snapshot();
    renderer.resize(800, 600);
    renderer.set_snapshot(snap);
    CHECK(renderer.point_count() == snap.points.size(), "all points uploaded");
    CHECK(renderer.segment_count() == 2, "two segments detected (gap preserved)");
    CHECK(renderer.revision() == snap.revision, "revision tracked");

    // Render a batch of frames; every frame must leave the GL error flag clean.
    const int frames = 180;
    int error_frames = 0;
    unsigned int last_tex = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        last_tex = renderer.render(1.0f / 60.0f);
        if (glGetError() != GL_NO_ERROR) ++error_frames;
        glfwPollEvents();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    const double fps = seconds > 1e-6 ? frames / seconds : 0.0;

    CHECK(error_frames == 0, "zero GL errors across all rendered frames");
    CHECK(last_tex != 0u, "composite produced a valid output texture");
    CHECK(renderer.last_frame_ms() >= 0.0f, "frame timing recorded");

    std::printf("GL renderer: %s | %d frames in %.3fs => %.1f FPS | last frame %.2f ms\n",
                cardiac::gl::renderer_string() ? cardiac::gl::renderer_string() : "unknown",
                frames, seconds, fps, renderer.last_frame_ms());
    if (fps < 30.0) {
        // Not fatal: a software rasterizer in CI is expected to be slow. The
        // desktop target is 30+ FPS on real GPU hardware.
        std::printf("NOTE: measured %.1f FPS (< 30). Likely a software/headless "
                    "rasterizer; on a discrete GPU this pipeline targets 30+ FPS.\n",
                    fps);
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    if (failures == 0) {
        std::printf("GL SMOKE TEST PASSED (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "%d GL failures / %d checks\n", failures, checks);
    return 1;
}

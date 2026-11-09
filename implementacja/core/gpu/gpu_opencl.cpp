#include "gpu_opencl.h"

#include <stdexcept>

#if !defined(EDGE_HAVE_OPENCL)

bool gpuCompiledIn() { return false; }
std::vector<GpuDeviceInfo> gpuDevices() { return {}; }
void gpuPrepare(const GrayImage&, const Mask&) {}
void runGpu(GpuKernel, const GrayImage&, const Mask&, const RunConfig&, RunResult&)
{
    throw std::runtime_error("program zbudowany bez OpenCL");
}

#else

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

const char* kSource = R"CLC(
#pragma OPENCL FP_CONTRACT OFF
#ifdef USE_FP64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#endif

// useDouble: L2 w double (dokladne dla kazdej maski, na kartach konsumenckich
// ~1/32 szybkosci float).  Float jest dokladny, gdy modul^2 < 2^24.
inline float magnitude(int sx, int sy, int norm, int useDouble)
{
    if (norm == 1) return (float)(abs(sx) + abs(sy));
    if (norm == 2) return (float)max(abs(sx), abs(sy));
#ifdef USE_FP64
    if (useDouble) {
        const double dx = (double)sx, dy = (double)sy;
        return (float)sqrt(dx * dx + dy * dy);
    }
#endif
    const float fx = (float)sx, fy = (float)sy;
    return sqrt(fx * fx + fy * fy);
}

inline uchar toByte(float v, float scale)
{
    const float t = v * scale;
    return t > 255.0f ? (uchar)255 : (uchar)(t + 0.5f);
}

// Maksimum w grupie roboczej (rozmiar = potega 2) + jeden atomic_max na grupe.
// Dodatnie float porownuja sie jak uint, wiec atomic_max na bitach dziala.
inline void groupMax(__local float* red, int lid, int n, float v, __global uint* maxbits)
{
    red[lid] = v;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s = n / 2; s > 0; s >>= 1) {
        if (lid < s) red[lid] = fmax(red[lid], red[lid + s]);
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (lid == 0) atomic_max(maxbits, as_uint(red[0]));
}

__kernel void conv_naive(__global const uchar* img, int w, int h,
                         __constant int* gx, __constant int* gy, int K, int norm, int useDouble,
                         int direct, float scale,
                         __global float* mag, __global uchar* out, __global uint* maxbits,
                         __local float* red, int ppw)
{
    // ppw = pikseli na work-item: work-item liczy piksele x0, x0 + G, x0 + 2G, ...
    // (G = globalny rozmiar w osi x), wiec sasiednie work-items czytaja sasiednie
    // piksele (odczyty laczone w transakcje pamieci) przy dowolnym ppw.
    const int x0 = get_global_id(0), y = get_global_id(1), G = get_global_size(0);
    const int lid = get_local_id(1) * get_local_size(0) + get_local_id(0);
    const int n = get_local_size(0) * get_local_size(1);
    float vmax = 0.0f;
    for (int j = 0; j < ppw; ++j) {
        const int x = x0 + j * G;
        if (x < w && y < h) {
            const int R = K / 2;
            int sx = 0, sy = 0;
            for (int ky = 0; ky < K; ++ky) {
                const int yy = clamp(y + ky - R, 0, h - 1);
                for (int kx = 0; kx < K; ++kx) {
                    const int xx = clamp(x + kx - R, 0, w - 1);
                    const int p = img[yy * w + xx];
                    sx += gx[ky * K + kx] * p;
                    sy += gy[ky * K + kx] * p;
                }
            }
            const float v = magnitude(sx, sy, norm, useDouble);
            if (direct) out[y * w + x] = toByte(v, scale);
            else        mag[y * w + x] = v;
            vmax = fmax(vmax, v);
        }
    }
    if (!direct) groupMax(red, lid, n, vmax, maxbits);
}

#ifdef STATIC_MASK
// Maska wkompilowana w program (odpowiednik V2 na CPU): KS, GXLIST, GYLIST
// z opcji kompilacji, wiec petle sa rozwijane, a mnozenia przez 0 znikaja.
__constant int GXC[KS * KS] = { GXLIST };
__constant int GYC[KS * KS] = { GYLIST };

__kernel void conv_static(__global const uchar* img, int w, int h, int norm, int useDouble,
                          int direct, float scale,
                          __global float* mag, __global uchar* out, __global uint* maxbits,
                          __local float* red)
{
    const int x = get_global_id(0), y = get_global_id(1);
    const int lid = get_local_id(1) * get_local_size(0) + get_local_id(0);
    const int n = get_local_size(0) * get_local_size(1);
    float v = 0.0f;
    if (x < w && y < h) {
        const int R = KS / 2;
        int sx = 0, sy = 0;
        #pragma unroll
        for (int ky = 0; ky < KS; ++ky) {
            const int yy = clamp(y + ky - R, 0, h - 1);
            #pragma unroll
            for (int kx = 0; kx < KS; ++kx) {
                const int p = img[yy * w + clamp(x + kx - R, 0, w - 1)];
                sx += GXC[ky * KS + kx] * p;
                sy += GYC[ky * KS + kx] * p;
            }
        }
        v = magnitude(sx, sy, norm, useDouble);
        if (direct) out[y * w + x] = toByte(v, scale);
        else        mag[y * w + x] = v;
    }
    if (!direct) groupMax(red, lid, n, v, maxbits);
}

// conv_static + kafel 16x16 z obrzezem R w pamieci lokalnej
__kernel void conv_local(__global const uchar* img, int w, int h, int norm, int useDouble,
                         int direct, float scale,
                         __global float* mag, __global uchar* out, __global uint* maxbits,
                         __local float* red, __local uchar* tile)
{
    const int R = KS / 2;
    const int lx = get_local_id(0), ly = get_local_id(1);
    const int lw = get_local_size(0), lh = get_local_size(1);
    const int tw = lw + 2 * R, th = lh + 2 * R;
    const int ox = get_group_id(0) * lw - R, oy = get_group_id(1) * lh - R;
    const int lid = ly * lw + lx, n = lw * lh;

    for (int i = lid; i < tw * th; i += n) {
        const int tx = i % tw, ty = i / tw;
        const int xx = clamp(ox + tx, 0, w - 1), yy = clamp(oy + ty, 0, h - 1);
        tile[i] = img[yy * w + xx];
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    const int x = get_global_id(0), y = get_global_id(1);
    float v = 0.0f;
    if (x < w && y < h) {
        int sx = 0, sy = 0;
        #pragma unroll
        for (int ky = 0; ky < KS; ++ky)
            #pragma unroll
            for (int kx = 0; kx < KS; ++kx) {
                const int p = tile[(ly + ky) * tw + (lx + kx)];
                sx += GXC[ky * KS + kx] * p;
                sy += GYC[ky * KS + kx] * p;
            }
        v = magnitude(sx, sy, norm, useDouble);
        if (direct) out[y * w + x] = toByte(v, scale);
        else        mag[y * w + x] = v;
    }
    if (!direct) groupMax(red, lid, n, v, maxbits);
}
#endif

__kernel void normalize(__global const float* mag, __global uchar* out, int n, float scale)
{
    const int i = get_global_id(0);
    if (i < n) out[i] = toByte(mag[i], scale);
}
)CLC";

constexpr int kWg = 16;   // grupa robocza 16 x 16

void check(cl_int err, const char* what)
{
    if (err != CL_SUCCESS)
        throw std::runtime_error(std::string("OpenCL: ") + what + " (kod " + std::to_string(err) + ")");
}

std::string deviceString(cl_device_id d, cl_device_info p)
{
    size_t sz = 0;
    clGetDeviceInfo(d, p, 0, nullptr, &sz);
    std::string s(sz, '\0');
    clGetDeviceInfo(d, p, sz, s.data(), nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string platformString(cl_platform_id pl, cl_platform_info p)
{
    size_t sz = 0;
    clGetPlatformInfo(pl, p, 0, nullptr, &sz);
    std::string s(sz, '\0');
    clGetPlatformInfo(pl, p, sz, s.data(), nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

double eventMs(cl_event e)
{
    cl_ulong t0 = 0, t1 = 0;
    clGetEventProfilingInfo(e, CL_PROFILING_COMMAND_START, sizeof(t0), &t0, nullptr);
    clGetEventProfilingInfo(e, CL_PROFILING_COMMAND_END, sizeof(t1), &t1, nullptr);
    return static_cast<double>(t1 - t0) * 1e-6;
}

struct Event {
    cl_event e = nullptr;
    ~Event() { if (e) clReleaseEvent(e); }
};

struct Ocl {
    cl_platform_id   platform = nullptr;
    cl_device_id     device   = nullptr;
    cl_context       ctx      = nullptr;
    cl_command_queue queue    = nullptr;
    cl_program       program  = nullptr;
    cl_kernel        kNaive = nullptr, kNorm = nullptr;
    struct MaskProgram { cl_program prog = nullptr; cl_kernel kStatic = nullptr, kLocal = nullptr; };
    std::map<std::string, MaskProgram> maskProgs;
    std::string      baseOpts;
    bool             fp64 = false;
    size_t           pixels = 0;
    cl_mem           bImg = nullptr, bMag = nullptr, bOut = nullptr, bMax = nullptr;
    std::map<std::string, std::pair<cl_mem, cl_mem>> maskBufs;

    Ocl()
    {
        cl_uint np = 0;
        check(clGetPlatformIDs(0, nullptr, &np), "clGetPlatformIDs");
        if (np == 0) throw std::runtime_error("OpenCL: brak platform");
        std::vector<cl_platform_id> ps(np);
        clGetPlatformIDs(np, ps.data(), nullptr);
        // Pierwsze urzadzenie GPU; NVIDIA ma pierwszenstwo (karta dedykowana)
        for (int pass = 0; pass < 2 && !device; ++pass)
            for (auto p : ps) {
                const std::string vendor = platformString(p, CL_PLATFORM_VENDOR);
                if (pass == 0 && vendor.find("NVIDIA") == std::string::npos) continue;
                cl_device_id d = nullptr;
                cl_uint nd = 0;
                if (clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &d, &nd) == CL_SUCCESS && nd > 0) {
                    platform = p;
                    device = d;
                    break;
                }
            }
        if (!device) throw std::runtime_error("OpenCL: brak urzadzenia GPU");

        fp64 = deviceString(device, CL_DEVICE_EXTENSIONS).find("cl_khr_fp64") != std::string::npos;
        cl_int err = CL_SUCCESS;
        ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
        check(err, "clCreateContext");
        queue = clCreateCommandQueue(ctx, device, CL_QUEUE_PROFILING_ENABLE, &err);
        check(err, "clCreateCommandQueue");
        baseOpts = "-cl-std=CL1.2 -cl-fp32-correctly-rounded-divide-sqrt";
        if (fp64) baseOpts += " -DUSE_FP64";
        program = build(baseOpts);
        kNaive = clCreateKernel(program, "conv_naive", &err); check(err, "clCreateKernel conv_naive");
        kNorm  = clCreateKernel(program, "normalize", &err);  check(err, "clCreateKernel normalize");
        cl_int e2 = CL_SUCCESS;
        bMax = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(cl_uint), nullptr, &e2);
        check(e2, "clCreateBuffer max");
    }

    cl_program build(const std::string& opts)
    {
        cl_int err = CL_SUCCESS;
        cl_program pr = clCreateProgramWithSource(ctx, 1, &kSource, nullptr, &err);
        check(err, "clCreateProgramWithSource");
        err = clBuildProgram(pr, 1, &device, opts.c_str(), nullptr, nullptr);
        if (err != CL_SUCCESS) {
            size_t sz = 0;
            clGetProgramBuildInfo(pr, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &sz);
            std::string log(sz, '\0');
            clGetProgramBuildInfo(pr, device, CL_PROGRAM_BUILD_LOG, sz, log.data(), nullptr);
            clReleaseProgram(pr);
            throw std::runtime_error("OpenCL: blad kompilacji kernela (" + opts + "):\n" + log);
        }
        return pr;
    }

    // Program z maska wkompilowana jako stale (osobny na kazda maske, kompilowany raz)
    MaskProgram& maskProgram(const Mask& m)
    {
        auto it = maskProgs.find(m.id);
        if (it != maskProgs.end()) return it->second;
        auto list = [](const std::vector<int>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
            return s;
        };
        const std::string opts = baseOpts + " -DSTATIC_MASK -DKS=" + std::to_string(m.size) +
                                 " -DGXLIST=" + list(m.gx) + " -DGYLIST=" + list(m.gy);
        MaskProgram mp;
        mp.prog = build(opts);
        cl_int err = CL_SUCCESS;
        mp.kStatic = clCreateKernel(mp.prog, "conv_static", &err); check(err, "clCreateKernel conv_static");
        mp.kLocal  = clCreateKernel(mp.prog, "conv_local", &err);  check(err, "clCreateKernel conv_local");
        return maskProgs[m.id] = mp;
    }

    ~Ocl()
    {
        for (auto& [id, mp] : maskProgs) {
            if (mp.kStatic) clReleaseKernel(mp.kStatic);
            if (mp.kLocal) clReleaseKernel(mp.kLocal);
            if (mp.prog) clReleaseProgram(mp.prog);
        }
        for (auto& [id, b] : maskBufs) { clReleaseMemObject(b.first); clReleaseMemObject(b.second); }
        for (cl_mem m : { bImg, bMag, bOut, bMax }) if (m) clReleaseMemObject(m);
        for (cl_kernel k : { kNaive, kNorm }) if (k) clReleaseKernel(k);
        if (program) clReleaseProgram(program);
        if (queue) clReleaseCommandQueue(queue);
        if (ctx) clReleaseContext(ctx);
    }

    void ensureBuffers(size_t n)
    {
        if (n <= pixels && bImg) return;
        for (cl_mem* m : { &bImg, &bMag, &bOut }) if (*m) { clReleaseMemObject(*m); *m = nullptr; }
        cl_int err = CL_SUCCESS;
        bImg = clCreateBuffer(ctx, CL_MEM_READ_ONLY, n, nullptr, &err);                 check(err, "bufor obrazu");
        bMag = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * sizeof(float), nullptr, &err); check(err, "bufor modulu");
        bOut = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n, nullptr, &err);                 check(err, "bufor wyniku");
        pixels = n;
    }

    std::pair<cl_mem, cl_mem> maskBuffers(const Mask& m)
    {
        auto it = maskBufs.find(m.id);
        if (it != maskBufs.end()) return it->second;
        cl_int err = CL_SUCCESS;
        const size_t bytes = m.gx.size() * sizeof(cl_int);
        cl_mem gx = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes,
                                   const_cast<int*>(m.gx.data()), &err);
        check(err, "bufor maski gx");
        cl_mem gy = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes,
                                   const_cast<int*>(m.gy.data()), &err);
        check(err, "bufor maski gy");
        return maskBufs[m.id] = { gx, gy };
    }
};

std::mutex g_mutex;
std::unique_ptr<Ocl> g_ocl;

Ocl& ocl()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_ocl) g_ocl = std::make_unique<Ocl>();
    return *g_ocl;
}

} // namespace

bool gpuCompiledIn() { return true; }

std::vector<GpuDeviceInfo> gpuDevices()
{
    std::vector<GpuDeviceInfo> out;
    cl_uint np = 0;
    if (clGetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0) return out;
    std::vector<cl_platform_id> ps(np);
    clGetPlatformIDs(np, ps.data(), nullptr);
    for (auto p : ps) {
        cl_uint nd = 0;
        if (clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 0, nullptr, &nd) != CL_SUCCESS || nd == 0) continue;
        std::vector<cl_device_id> ds(nd);
        clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, nd, ds.data(), nullptr);
        for (auto d : ds) {
            GpuDeviceInfo g;
            g.platform = platformString(p, CL_PLATFORM_NAME);
            g.device   = deviceString(d, CL_DEVICE_NAME);
            g.version  = deviceString(d, CL_DEVICE_VERSION);
            g.driver   = deviceString(d, CL_DRIVER_VERSION);
            g.fp64     = deviceString(d, CL_DEVICE_EXTENSIONS).find("cl_khr_fp64") != std::string::npos;
            cl_ulong mem = 0;
            cl_uint cu = 0;
            clGetDeviceInfo(d, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, nullptr);
            clGetDeviceInfo(d, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, nullptr);
            g.globalMemMB  = static_cast<long long>(mem / (1024 * 1024));
            g.computeUnits = static_cast<int>(cu);
            out.push_back(g);
        }
    }
    return out;
}

void gpuPrepare(const GrayImage& img, const Mask& mask)
{
    Ocl& o = ocl();
    o.ensureBuffers(static_cast<size_t>(img.width) * img.height);
    o.maskBuffers(mask);
    o.maskProgram(mask);
}

void runGpu(GpuKernel kind, const GrayImage& img, const Mask& mask, const RunConfig& cfg, RunResult& r)
{
    Ocl& o = ocl();
    const int w = img.width, h = img.height;
    const size_t n = static_cast<size_t>(w) * h;
    o.ensureBuffers(n);
    const auto [bgx, bgy] = o.maskBuffers(mask);
    if (r.output.size() != n) r.output.resize(n);
    r.threadsUsed = 1;

    const bool global = cfg.scaling == Scaling::GlobalMax;
    const cl_int direct = global ? 0 : 1;
    const float  scale0 = global ? 0.0f : directScale(mask, cfg.norm, cfg.scaling);
    const cl_int K = mask.size, norm = static_cast<cl_int>(cfg.norm == Norm::L1 ? 1 : cfg.norm == Norm::Linf ? 2 : 0);
    const cl_int cw = w, ch = h;
    const cl_uint zero = 0;
    // auto: double tylko gdy float nie bylby dokladny; fp32: zawsze float
    const float tmax = theoreticalMax(mask, Norm::L2);
    const bool floatExact = static_cast<double>(tmax) * tmax < 16777216.0;
    const cl_int useDouble = (o.fp64 && !cfg.gpuFp32 && !floatExact) ? 1 : 0;

    Event evUp, evFill, evConv, evMax, evNorm, evDown;
    check(clEnqueueWriteBuffer(o.queue, o.bImg, CL_FALSE, 0, n, img.data.data(), 0, nullptr, &evUp.e), "upload");
    check(clEnqueueWriteBuffer(o.queue, o.bMax, CL_FALSE, 0, sizeof(zero), &zero, 0, nullptr, &evFill.e), "zerowanie maks.");

    Ocl::MaskProgram& mp = o.maskProgram(mask);
    cl_kernel k = kind == GpuKernel::Naive ? o.kNaive : kind == GpuKernel::Static ? mp.kStatic : mp.kLocal;
    int a = 0;
    check(clSetKernelArg(k, a++, sizeof(cl_mem), &o.bImg), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_int), &cw), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_int), &ch), "arg");
    if (kind == GpuKernel::Naive) {
        check(clSetKernelArg(k, a++, sizeof(cl_mem), &bgx), "arg");
        check(clSetKernelArg(k, a++, sizeof(cl_mem), &bgy), "arg");
        check(clSetKernelArg(k, a++, sizeof(cl_int), &K), "arg");
    }
    check(clSetKernelArg(k, a++, sizeof(cl_int), &norm), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_int), &useDouble), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_int), &direct), "arg");
    check(clSetKernelArg(k, a++, sizeof(float), &scale0), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_mem), &o.bMag), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_mem), &o.bOut), "arg");
    check(clSetKernelArg(k, a++, sizeof(cl_mem), &o.bMax), "arg");
    // Rozmiar grupy roboczej i liczba pikseli na work-item – parametry tylko kernela naiwnego (W5)
    const bool naive = kind == GpuKernel::Naive;
    const size_t wgx = naive ? static_cast<size_t>(std::max(1, cfg.gpuWgX)) : kWg;
    const size_t wgy = naive ? static_cast<size_t>(std::max(1, cfg.gpuWgY)) : kWg;
    const cl_int ppw = naive ? std::max(1, cfg.gpuPpw) : 1;
    check(clSetKernelArg(k, a++, sizeof(float) * wgx * wgy, nullptr), "arg local red");
    if (naive) check(clSetKernelArg(k, a++, sizeof(cl_int), &ppw), "arg ppw");
    if (kind == GpuKernel::Local) {
        const size_t tile = static_cast<size_t>(kWg + 2 * (K / 2)) * (kWg + 2 * (K / 2));
        check(clSetKernelArg(k, a++, tile, nullptr), "arg local tile");
    }
    const size_t itemsX = (static_cast<size_t>(w) + ppw - 1) / ppw;
    const size_t gws[2] = { (itemsX + wgx - 1) / wgx * wgx,
                            (static_cast<size_t>(h) + wgy - 1) / wgy * wgy };
    const size_t lws[2] = { wgx, wgy };
    r.gpuWorkItems = static_cast<long long>(gws[0] * gws[1]);
    check(clEnqueueNDRangeKernel(o.queue, k, 2, nullptr, gws, lws, 0, nullptr, &evConv.e), "kernel splotu");

    double kernelMs = 0.0, downMs = 0.0;
    if (global) {
        cl_uint maxBits = 0;
        check(clEnqueueReadBuffer(o.queue, o.bMax, CL_TRUE, 0, sizeof(maxBits), &maxBits, 0, nullptr, &evMax.e), "odczyt maks.");
        float maxV = 0.0f;
        std::memcpy(&maxV, &maxBits, sizeof(maxV));
        r.maxGradient = maxV;
        const float scale = globalScale(maxV);
        const cl_int cn = static_cast<cl_int>(n);
        check(clSetKernelArg(o.kNorm, 0, sizeof(cl_mem), &o.bMag), "arg");
        check(clSetKernelArg(o.kNorm, 1, sizeof(cl_mem), &o.bOut), "arg");
        check(clSetKernelArg(o.kNorm, 2, sizeof(cl_int), &cn), "arg");
        check(clSetKernelArg(o.kNorm, 3, sizeof(float), &scale), "arg");
        const size_t g1 = (n + 255) / 256 * 256, l1 = 256;
        check(clEnqueueNDRangeKernel(o.queue, o.kNorm, 1, nullptr, &g1, &l1, 0, nullptr, &evNorm.e), "kernel normalizacji");
    }
    check(clEnqueueReadBuffer(o.queue, o.bOut, CL_TRUE, 0, n, r.output.data(), 0, nullptr, &evDown.e), "download");
    check(clFinish(o.queue), "clFinish");

    kernelMs = eventMs(evConv.e) + (evNorm.e ? eventMs(evNorm.e) : 0.0);
    downMs   = eventMs(evDown.e) + (evMax.e ? eventMs(evMax.e) : 0.0);
    r.stagesMs["upload"]   = eventMs(evUp.e) + eventMs(evFill.e);
    r.stagesMs["kernel"]   = kernelMs;
    r.stagesMs["download"] = downMs;
}

#endif

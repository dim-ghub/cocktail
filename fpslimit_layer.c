/*
 * Minimal Vulkan implicit layer that caps the frame rate at vkQueuePresentKHR.
 * The cap is read from a plain-text file (a number, 0 = unlimited) that is
 * re-checked every 50 ms, including while waiting out a long frame, so it can
 * be changed while the game runs.
 *
 * Control file: $SOBER_FPS_FILE, else $XDG_CACHE_HOME/sober-fpscap
 * (inside Sober's sandbox: ~/.var/app/org.vinegarhq.Sober/cache/sober-fpscap)
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

#define EXPORT __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_SOBER_fpslimit"
#define MAX_ENTRIES 32

static int is_target_process(void)
{
    const char *dis = getenv("DISABLE_COCKTAIL_FPSLIMIT");
    if (dis && (!strcmp(dis, "1") || !strcasecmp(dis, "true"))) return 0;
    dis = getenv("DISABLE_SOBER_FPSLIMIT");
    if (dis && (!strcmp(dis, "1") || !strcasecmp(dis, "true"))) return 0;

    const char *e = getenv("COCKTAIL_FPSLIMIT");
    if (e && (!strcmp(e, "1") || !strcasecmp(e, "true"))) return 1;
    e = getenv("SOBER_FPSLIMIT");
    if (e && (!strcmp(e, "1") || !strcasecmp(e, "true"))) return 1;

    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        if (strcasestr(buf, "mocktail") || strcasestr(buf, "nightcap") ||
            strcasestr(buf, "sober") || strcasestr(buf, "roblox")) {
            return 1;
        }
    }

    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len > 0) {
            buf[len] = '\0';
            for (ssize_t i = 0; i < len; i++) {
                if (buf[i] == '\0') buf[i] = ' ';
            }
            if (strcasestr(buf, "mocktail") || strcasestr(buf, "nightcap") ||
                strcasestr(buf, "sober") || strcasestr(buf, "roblox")) {
                return 1;
            }
        }
    }

    return 0;
}

static int g_target_checked = 0;
static int g_is_target = 0;

static inline int check_target(void)
{
    if (!g_target_checked) {
        g_is_target = is_target_process();
        g_target_checked = 1;
    }
    return g_is_target;
}

/* ---- dispatch bookkeeping, keyed by the loader dispatch pointer ---- */

typedef struct {
    void *key;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy_instance;
} InstanceData;

typedef struct {
    void *key;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkQueuePresentKHR queue_present;
} DeviceData;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static InstanceData g_instances[MAX_ENTRIES];
static DeviceData g_devices[MAX_ENTRIES];

static inline void *dkey(const void *handle) { return *(void **)handle; }

static InstanceData *find_instance(void *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (g_instances[i].key == key) return &g_instances[i];
    return NULL;
}

static DeviceData *find_device(void *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (g_devices[i].key == key) return &g_devices[i];
    return NULL;
}

/* ---- limiter ---- */

#define CHECK_NS 50000000ull   /* how often the control file is polled */
#define SPIN_NS  500000ull     /* busy-wait the last 0.5 ms for precision */

static char g_path[4096];
static uint64_t g_interval_ns;      /* 0 = unlimited */
static uint64_t g_next_check_ns;
static struct timespec g_last_mtime;
static uint64_t g_last_frame_ns;
static pthread_mutex_t g_limit_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void init_path(void)
{
    const char *p = getenv("COCKTAIL_FPS_FILE");
    if (!p || !*p) p = getenv("SOBER_FPS_FILE");
    if (p && *p) {
        snprintf(g_path, sizeof g_path, "%s", p);
        return;
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache && *cache)
        snprintf(g_path, sizeof g_path, "%s/cocktail-fpscap", cache);
    else
        snprintf(g_path, sizeof g_path, "%s/.cache/cocktail-fpscap",
                 getenv("HOME") ? getenv("HOME") : "/tmp");
}

static void refresh_cap(uint64_t now)
{
    if (now < g_next_check_ns) return;
    g_next_check_ns = now + CHECK_NS;

    struct stat st;
    if (stat(g_path, &st) != 0) {
        /* Fall back to sober-fpscap if cocktail-fpscap doesn't exist yet */
        const char *cache = getenv("XDG_CACHE_HOME");
        char fallback[4096];
        if (cache && *cache)
            snprintf(fallback, sizeof fallback, "%s/sober-fpscap", cache);
        else
            snprintf(fallback, sizeof fallback, "%s/.cache/sober-fpscap",
                     getenv("HOME") ? getenv("HOME") : "/tmp");
        if (stat(fallback, &st) == 0) {
            snprintf(g_path, sizeof g_path, "%s", fallback);
        } else {
            g_interval_ns = 0;
            g_last_mtime = (struct timespec){0};
            return;
        }
    }
    if (st.st_mtim.tv_sec == g_last_mtime.tv_sec &&
        st.st_mtim.tv_nsec == g_last_mtime.tv_nsec)
        return;
    g_last_mtime = st.st_mtim;

    char buf[32] = {0};
    int fd = open(g_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;

    double fps = strtod(buf, NULL);
    g_interval_ns = fps > 0.0 ? (uint64_t)(1e9 / fps) : 0;
}

/* SOBER_FPS_STATS=1 or COCKTAIL_FPS_STATS=1 prints the delivered frame rate to stderr once a second. */
static int g_stats = -1;
static uint64_t g_stats_start_ns;
static unsigned g_stats_frames;

static void count_frame(uint64_t now)
{
    if (g_stats < 0) {
        const char *s = getenv("COCKTAIL_FPS_STATS");
        if (!s || !*s) s = getenv("SOBER_FPS_STATS");
        g_stats = s && *s == '1';
        g_stats_start_ns = now;
    }
    if (!g_stats) return;
    g_stats_frames++;
    if (now - g_stats_start_ns >= 1000000000ull) {
        fprintf(stderr, "cocktail-fpslimit: %.1f fps (cap %s%.0f)\n",
                g_stats_frames * 1e9 / (now - g_stats_start_ns),
                g_interval_ns ? "" : "off ",
                g_interval_ns ? 1e9 / g_interval_ns : 0.0);
        g_stats_frames = 0;
        g_stats_start_ns = now;
    }
}

static void sleep_until(uint64_t t)
{
    struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static void limit_frame(void)
{
    uint64_t now = now_ns();
    count_frame(now);

    /* Wait in slices so a new cap (e.g. going from 1 fps to 120) applies
     * mid-wait instead of after the current long frame finishes. */
    for (;;) {
        refresh_cap(now);
        if (g_interval_ns == 0) break;

        uint64_t target = g_last_frame_ns + g_interval_ns;
        if (now >= target) break;

        if (target - now > SPIN_NS) {
            uint64_t wake = target - SPIN_NS;
            if (wake - now > CHECK_NS) wake = now + CHECK_NS;
            sleep_until(wake);
        } else {
            while (now_ns() < target)
                ;
            g_last_frame_ns = target;
            return;
        }
        now = now_ns();
    }
    /* Uncapped, or running slower than the cap: don't accumulate debt. */
    g_last_frame_ns = now;
}

/* ---- intercepted entry points ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    pthread_mutex_lock(&g_lock);
    DeviceData *d = find_device(dkey(queue));
    PFN_vkQueuePresentKHR next = d ? d->queue_present : NULL;
    pthread_mutex_unlock(&g_lock);

    if (check_target()) {
        pthread_mutex_lock(&g_limit_lock);
        limit_frame();
        pthread_mutex_unlock(&g_limit_lock);
    }
    return next ? next(queue, info) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci,
                     const VkAllocationCallbacks *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&g_lock);
    InstanceData *slot = find_instance(NULL);
    if (slot) {
        slot->key = dkey(*out);
        slot->gipa = gipa;
        slot->destroy_instance =
            (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    }
    if (check_target()) {
        if (!g_path[0]) init_path();
        if (getenv("COCKTAIL_FPS_STATS") || getenv("SOBER_FPS_STATS"))
            g_stats = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    pthread_mutex_lock(&g_lock);
    InstanceData *d = find_instance(dkey(instance));
    PFN_vkDestroyInstance next = d ? d->destroy_instance : NULL;
    if (d) memset(d, 0, sizeof *d);
    pthread_mutex_unlock(&g_lock);
    if (next) next(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                   const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create =
        (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = create(phys, ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&g_lock);
    DeviceData *slot = find_device(NULL);
    if (slot) {
        slot->key = dkey(*out);
        slot->gdpa = gdpa;
        slot->destroy_device = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
        slot->queue_present =
            (PFN_vkQueuePresentKHR)gdpa(*out, "vkQueuePresentKHR");
    }
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    pthread_mutex_lock(&g_lock);
    DeviceData *d = find_device(dkey(device));
    PFN_vkDestroyDevice next = d ? d->destroy_device : NULL;
    if (d) memset(d, 0, sizeof *d);
    pthread_mutex_unlock(&g_lock);
    if (next) next(device, alloc);
}

/* ---- proc-addr plumbing ---- */

static PFN_vkVoidFunction device_hook(const char *name)
{
    if (!check_target()) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return NULL; /* set below */
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)layer_DestroyDevice;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)layer_QueuePresentKHR;
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
sober_fps_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)sober_fps_GetDeviceProcAddr;
    PFN_vkVoidFunction f = device_hook(name);
    if (f) return f;

    pthread_mutex_lock(&g_lock);
    DeviceData *d = find_device(dkey(device));
    PFN_vkGetDeviceProcAddr next = d ? d->gdpa : NULL;
    pthread_mutex_unlock(&g_lock);
    return next ? next(device, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
sober_fps_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)sober_fps_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)sober_fps_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_CreateDevice;
    PFN_vkVoidFunction f = device_hook(name);
    if (f) return f;

    if (!instance) return NULL;
    pthread_mutex_lock(&g_lock);
    InstanceData *d = find_instance(dkey(instance));
    PFN_vkGetInstanceProcAddr next = d ? d->gipa : NULL;
    pthread_mutex_unlock(&g_lock);
    return next ? next(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = sober_fps_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = sober_fps_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}

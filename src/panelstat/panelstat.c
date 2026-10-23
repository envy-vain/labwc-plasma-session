// panelstat: one tiny exec per LXQt panel refresh instead of sh + awk/cat/date chains.
// usage: panelstat temp | watts | eco
//   temp   CPU temperature (Intel coretemp, AMD k10temp/zenpower; looked up by name: hwmon numbers
//          change between boots)
//   watts  whole laptop (first battery's power_now, + while charging) and CPU package power (RAPL delta
//          since the previous call, kept in $XDG_RUNTIME_DIR/panelstat-watts.last). Either part is left
//          out when the machine has no battery or the kernel keeps RAPL root-only.
//   eco    ultra-low-power state icon (github.com/envy-vain/thinkpad-l13-linux)
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long readll(const char *path)
{
    FILE *f = fopen(path, "re");
    long long v = -1;
    if (f) {
        if (fscanf(f, "%lld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static int reads(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "re");
    buf[0] = 0;
    if (!f)
        return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return ok;
}

static int temp(void)
{
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    long long mc = -1; // millidegrees
    char p[300], name[64];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        if (reads(p, name, sizeof name)
            && (!strcmp(name, "coretemp") || !strcmp(name, "k10temp") || !strcmp(name, "zenpower"))) {
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp1_input", e->d_name);
            mc = readll(p);
            break;
        }
    }
    if (d)
        closedir(d);
    if (mc >= 0) // no Intel/AMD CPU sensor (VM, ARM...): leave the panel label empty
        printf("%s %lld°C", mc >= 85000 ? "󰸁" : "󰔏", mc / 1000);
    putchar('\n');
    return 0;
}

// First system battery (BAT0, BAT1, CMB0...), or 0 if there is none. Wireless mice/keyboards also show up
// as type Battery, with scope Device: skip those.
static int battery(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    char p[300], type[32], scope[32];
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if (!reads(p, type, sizeof type) || strcmp(type, "Battery"))
            continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", e->d_name);
        if (!reads(p, scope, sizeof scope) || strcmp(scope, "Device")) {
            snprintf(out, n, "/sys/class/power_supply/%s", e->d_name);
            found = 1;
        }
    }
    if (d)
        closedir(d);
    return found;
}

static int watts(void)
{
    const char *rapl = "/sys/class/powercap/intel-rapl:0"; // AMD exposes its RAPL here too
    char bat[300], p[400], status[32] = "";
    const char *sep = "";
    if (battery(bat, sizeof bat)) {
        snprintf(p, sizeof p, "%s/power_now", bat);
        double w = readll(p) / 1e6;
        if (w < 0) { // some batteries only report current and voltage
            snprintf(p, sizeof p, "%s/current_now", bat);
            long long ua = readll(p);
            snprintf(p, sizeof p, "%s/voltage_now", bat);
            long long uv = readll(p);
            w = ua >= 0 && uv >= 0 ? ua / 1e6 * (uv / 1e6) : -1;
        }
        snprintf(p, sizeof p, "%s/status", bat);
        reads(p, status, sizeof status);
        if (w >= 0) {
            printf("%s%.1f W", strcmp(status, "Charging") ? "" : "+", w);
            sep = "  ";
        }
    }

    snprintf(p, sizeof p, "%s/energy_uj", rapl);
    long long e = readll(p);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    const char *rt = getenv("XDG_RUNTIME_DIR"); // private dir; never a shared /tmp file
    if (!rt) {
        putchar('\n');
        return 0;
    }
    char state[256];
    snprintf(state, sizeof state, "%s/panelstat-watts.last", rt);
    FILE *f = fopen(state, "re");
    long long le, lt;
    if (f && e >= 0 && fscanf(f, "%lld %lld", &le, &lt) == 2 && now > lt) {
        long long de = e - le;
        if (de < 0) { // counter wrapped
            snprintf(p, sizeof p, "%s/max_energy_range_uj", rapl);
            de += readll(p);
        }
        printf("%sCPU %.1f W", sep, de / 1e6 / ((now - lt) / 1e9));
    }
    if (f)
        fclose(f);
    if (e >= 0 && (f = fopen(state, "we"))) {
        fprintf(f, "%lld %lld\n", e, now);
        fclose(f);
    }
    putchar('\n');
    return 0;
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "temp"))
        return temp();
    if (!strcmp(m, "watts"))
        return watts();
    if (!strcmp(m, "eco")) {
        puts(access("/run/ultra-low-power", F_OK) == 0 ? "󰌪 Eco" : "󱐋");
        return 0;
    }
    fputs("usage: panelstat temp|watts|eco\n", stderr);
    return 2;
}

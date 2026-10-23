#include "sysinfo.h"
#include "metrics.h"

typedef struct WftProcPowerInfo {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} WftProcPowerInfo;


static FILE *open_in(const WCHAR *dir, const WCHAR *name) {
    WCHAR p[WFT_PATH_CCH]; wft_wsnprintf(p, WFT_PATH_CCH, L"%ls\\%ls", dir, name);
    return plat_fopen_w(p);
}

static bool reg_str(HKEY root, const WCHAR *key, const WCHAR *name, WCHAR *out, DWORD cch) {
    DWORD bytes = cch * sizeof(WCHAR), type = 0;
    out[0] = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, out, &bytes) != ERROR_SUCCESS) { out[0] = 0; return false; }
    out[cch - 1] = 0;
    return true;
}
static void reg_line(FILE *f, HKEY root, const WCHAR *key, const WCHAR *name, const char *label) {
    WCHAR v[512];
    if (reg_str(root, key, name, v, 512)) { fprintf(f, "%s: ", label); wft_write_utf8(f, v); fputs("\r\n", f); }
}

static const char *arch_name(WORD a) {
    switch (a) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        case PROCESSOR_ARCHITECTURE_ARM: return "ARM";
        default: return "unknown";
    }
}

/* system_info.txt */
void sysinfo_write_system(const WCHAR *dir, const Config *cfg) {
    FILE *f = open_in(dir, L"system_info.txt"); if (!f) return;
    fputs("System snapshot\r\n===============\r\n", f);
    WCHAR computer[256] = {0}, user[256] = {0}; DWORD nc = 256, nu = 256;
    GetComputerNameW(computer, &nc); GetUserNameW(user, &nu);
    fputs("Computer: ", f); wft_write_utf8(f, cfg->redact ? L"<redacted>" : computer);
    fputs("\r\nUser: ", f);    wft_write_utf8(f, cfg->redact ? L"<redacted>" : user); fputs("\r\n", f);
    const WCHAR *cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    reg_line(f, HKEY_LOCAL_MACHINE, cv, L"ProductName", "Windows");
    reg_line(f, HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", "Display version");
    reg_line(f, HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber", "Build");
    DWORD ubr = 0, sz = sizeof ubr;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"UBR", RRF_RT_REG_DWORD, NULL, &ubr, &sz) == ERROR_SUCCESS) fprintf(f, "Build revision (UBR): %lu\r\n", (unsigned long)ubr);
    reg_line(f, HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", "CPU");
    const WCHAR *bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"SystemManufacturer", "System manufacturer");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"SystemProductName", "System model");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"BaseBoardManufacturer", "Board manufacturer");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"BaseBoardProduct", "Board model");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"BIOSVendor", "BIOS vendor");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"BIOSVersion", "BIOS version");
    reg_line(f, HKEY_LOCAL_MACHINE, bios, L"BIOSReleaseDate", "BIOS date");
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    fprintf(f, "Architecture: %s\r\nLogical processors: %lu (processor groups: %u)\r\nPage size: %lu bytes\r\n", arch_name(si.wProcessorArchitecture),
            (unsigned long)si.dwNumberOfProcessors, (unsigned)GetActiveProcessorGroupCount(), (unsigned long)si.dwPageSize);
    MEMORYSTATUSEX ms; memset(&ms, 0, sizeof ms); ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        fprintf(f, "Memory load: %lu%%\r\nPhysical memory: %.2f GB total, %.2f GB available\r\nCommit/pagefile: %.2f GB total, %.2f GB available\r\n",
                (unsigned long)ms.dwMemoryLoad, (double)ms.ullTotalPhys / 1073741824.0, (double)ms.ullAvailPhys / 1073741824.0,
                (double)ms.ullTotalPageFile / 1073741824.0, (double)ms.ullAvailPageFile / 1073741824.0);
    ULONGLONG up = GetTickCount64();
    fprintf(f, "Uptime: %.2f hours (%llu ms). Note: with Fast Startup a normal shutdown does not reset this.\r\n", (double)up / 3600000.0, (unsigned long long)up);
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        fprintf(f, "AC power: %s\r\n", ps.ACLineStatus == 1 ? "online" : (ps.ACLineStatus == 0 ? "offline" : "unknown"));
        if (ps.BatteryLifePercent != 255) fprintf(f, "Battery: %u%%\r\n", (unsigned)ps.BatteryLifePercent);
    }
    fclose(f);
}

/* drives.csv */
void sysinfo_write_drives(const WCHAR *dir, DriveFacts *df) {
    df->lowest_free_pct = df->lowest_free_gb = WFT_NA; df->lowest_drive[0] = 0;
    FILE *f = open_in(dir, L"drives.csv"); if (!f) return;
    fputs("drive,type,volume_label,filesystem,total_GB,free_GB,free_pct,bytes_per_cluster,note\r\n", f);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        WCHAR root[4] = { (WCHAR)(L'A' + i), L':', L'\\', 0 };
        UINT type = GetDriveTypeW(root);
        /* Network and optical drives can block for tens of seconds when offline/spinning up: never touch them mid-incident. */
        if (type == DRIVE_REMOTE || type == DRIVE_CDROM || type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) {
            fprintf(f, "%c,%u,\"\",\"\",,,,,skipped (network/optical/unknown)\r\n", 'A' + i, type);
            continue;
        }
        ULARGE_INTEGER avail, total, freeb;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) continue;
        WCHAR label[256] = {0}, fs[64] = {0}; DWORD ser = 0, mc = 0, fl = 0, spc = 0, bps = 0, fc = 0, tc = 0;
        GetVolumeInformationW(root, label, 256, &ser, &mc, &fl, fs, 64);
        GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc);
        double pct = total.QuadPart ? (double)freeb.QuadPart * 100.0 / (double)total.QuadPart : 0.0;
        double fgb = (double)freeb.QuadPart / 1073741824.0;
        fprintf(f, "%c,%u,", 'A' + i, type); wft_csv_text(f, label); fputc(',', f); wft_csv_text(f, fs);
        fprintf(f, ",%.2f,%.2f,%.2f,%llu,\r\n", (double)total.QuadPart / 1073741824.0, fgb, pct, (unsigned long long)spc * bps);
        if (type == DRIVE_FIXED && total.QuadPart && (!WFT_HAS(df->lowest_free_pct) || pct < df->lowest_free_pct)) {
            df->lowest_free_pct = pct; df->lowest_free_gb = fgb; wft_snprintf(df->lowest_drive, sizeof df->lowest_drive, "%c:", 'A' + i);
        }
    }
    fclose(f);
}

/* loaded_drivers.csv */
static void driver_path(const WCHAR *raw, WCHAR *out, size_t cch) {
    if (wft_wcs_istarts(raw, L"\\SystemRoot\\")) {
        WCHAR win[MAX_PATH]; UINT n = GetWindowsDirectoryW(win, MAX_PATH);
        wft_wsnprintf(out, cch, L"%ls\\%ls", n ? win : L"C:\\Windows", raw + 12);
    } else if (wft_wcs_istarts(raw, L"\\??\\")) wft_wsnprintf(out, cch, L"%ls", raw + 4);
    else wft_wsnprintf(out, cch, L"%ls", raw);
}

static void file_version(const WCHAR *path, WCHAR *company, size_t cc, WCHAR *ver, size_t cv) {
    company[0] = ver[0] = 0;
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    if (!sz || sz > (1u << 20)) return;
    void *buf = malloc(sz);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO *ffi = NULL; UINT len = 0;
        if (VerQueryValueW(buf, L"\\", (LPVOID *)&ffi, &len) && ffi && len >= sizeof *ffi)
            wft_wsnprintf(ver, cv, L"%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        struct { WORD lang, cp; } *tr = NULL; UINT tl = 0;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (LPVOID *)&tr, &tl) && tr && tl >= 4) {
            WCHAR sub[64]; WCHAR *val = NULL; UINT vl = 0;
            wft_wsnprintf(sub, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", tr[0].lang, tr[0].cp);
            if (VerQueryValueW(buf, sub, (LPVOID *)&val, &vl) && val) wft_wsnprintf(company, cc, L"%ls", val);
        }
    }
    free(buf);
}

void sysinfo_write_drivers(const WCHAR *dir, const Config *cfg) {
    FILE *f = open_in(dir, L"loaded_drivers.csv"); if (!f) return;
    fputs("base,module,path,company,file_version,file_date,third_party\r\n", f);
    DWORD needed = 0;
    LPVOID *drivers = (LPVOID *)calloc(4096, sizeof(LPVOID));
    if (drivers && EnumDeviceDrivers(drivers, 4096 * sizeof(LPVOID), &needed)) {
        DWORD count = needed / sizeof(LPVOID); if (count > 4096) count = 4096;
        ULONGLONG t0 = GetTickCount64();
        for (DWORD i = 0; i < count; ++i) {
            WCHAR name[MAX_PATH] = {0}, raw[WFT_PATH_CCH] = {0}, full[WFT_PATH_CCH] = {0}, company[128] = {0}, ver[48] = {0};
            GetDeviceDriverBaseNameW(drivers[i], name, MAX_PATH);
            GetDeviceDriverFileNameW(drivers[i], raw, WFT_PATH_CCH);
            driver_path(raw, full, WFT_PATH_CCH);
            WIN32_FILE_ATTRIBUTE_DATA fad; char date[16] = "";
            if (GetTickCount64() - t0 < 3000) {                          /* bounded: version lookups touch the disk */
                file_version(full, company, 128, ver, 48);
                if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) {
                    SYSTEMTIME st; if (FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) wft_snprintf(date, sizeof date, "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
                }
            }
            bool ms = wft_wcs_icontains(company, L"Microsoft");
            fprintf(f, "0x%p,", drivers[i]); wft_csv_text(f, name); fputc(',', f);
            if (cfg->redact) wft_redact_user_path(full, WFT_PATH_CCH);
            wft_csv_text(f, full); fputc(',', f); wft_csv_text(f, company);
            fputc(',', f); wft_csv_text(f, ver); fprintf(f, ",%s,%s\r\n", date, company[0] ? (ms ? "no" : "yes") : "unknown");
        }
    }
    free(drivers);
    fclose(f);
}

/* storage_devices.csv */
static const char *bus_name(unsigned t) {
    static const char *const n[] = { "Unknown", "SCSI", "ATAPI", "ATA", "1394", "SSA", "FibreChannel", "USB", "RAID", "iSCSI", "SAS", "SATA", "SD", "MMC", "Virtual", "FileBackedVirtual", "StorageSpaces", "NVMe" };
    return t < sizeof n / sizeof n[0] ? n[t] : "Other";
}

static void desc_str(const BYTE *buf, DWORD bytes, DWORD off, char *out, size_t cch) {
    out[0] = 0;
    if (!off || off >= bytes) return;
    size_t i = 0;
    while (off + i < bytes && buf[off + i] && i + 1 < cch) { out[i] = (char)buf[off + i]; ++i; }   /* bounded: descriptor strings may be unterminated */
    out[i] = 0;
    while (i > 0 && out[i - 1] == ' ') out[--i] = 0;
}

void sysinfo_write_storage(const WCHAR *dir, const Config *cfg) {
    FILE *f = open_in(dir, L"storage_devices.csv"); if (!f) return;
    fputs("physical_index,device_number,bus_type,bus_name,removable,command_queueing,incurs_seek_penalty_hdd,vendor,model,firmware,serial\r\n", f);
    for (DWORD i = 0; i < 32; ++i) {
        WCHAR dev[64]; wft_wsnprintf(dev, 64, L"\\\\.\\PhysicalDrive%lu", (unsigned long)i);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        BYTE buf[4096]; memset(buf, 0, sizeof buf);
        STORAGE_PROPERTY_QUERY q; memset(&q, 0, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const STORAGE_DEVICE_DESCRIPTOR *d = (const STORAGE_DEVICE_DESCRIPTOR *)buf;
            STORAGE_DEVICE_NUMBER num; memset(&num, 0, sizeof num); DWORD gn = 0; DWORD devno = i;
            if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &num, sizeof num, &gn, NULL)) devno = num.DeviceNumber;
            int seek = -1;
            STORAGE_PROPERTY_QUERY q2; memset(&q2, 0, sizeof q2); q2.PropertyId = StorageDeviceSeekPenaltyProperty; q2.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR sp; memset(&sp, 0, sizeof sp); DWORD gs = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof q2, &sp, sizeof sp, &gs, NULL) && gs >= sizeof sp) seek = sp.IncursSeekPenalty ? 1 : 0;
            char vendor[64], model[96], fw[32], serial[64];
            desc_str(buf, got, d->VendorIdOffset, vendor, sizeof vendor);
            desc_str(buf, got, d->ProductIdOffset, model, sizeof model);
            desc_str(buf, got, d->ProductRevisionOffset, fw, sizeof fw);
            desc_str(buf, got, d->SerialNumberOffset, serial, sizeof serial);
            fprintf(f, "%lu,%lu,%u,%s,%u,%u,%d,", (unsigned long)i, (unsigned long)devno, (unsigned)d->BusType, bus_name((unsigned)d->BusType),
                    (unsigned)d->RemovableMedia, (unsigned)d->CommandQueueing, seek);
            wft_csv_utf8(f, vendor); fputc(',', f); wft_csv_utf8(f, model); fputc(',', f); wft_csv_utf8(f, fw); fputc(',', f);
            wft_csv_utf8(f, cfg->redact ? "<redacted>" : serial); fputs("\r\n", f);
        }
        CloseHandle(h);
    }
    fclose(f);
}

/* displays.csv + display_drivers.csv */
void sysinfo_write_displays(const WCHAR *dir, const Config *cfg) {
    FILE *f = open_in(dir, L"displays.csv");
    if (f) {
        fputs("index,state_flags,device_name,description,device_id\r\n", f);
        for (DWORD i = 0; i < 64; ++i) {
            DISPLAY_DEVICEW d; memset(&d, 0, sizeof d); d.cb = sizeof d;
            if (!EnumDisplayDevicesW(NULL, i, &d, 0)) break;
            fprintf(f, "%lu,0x%08lx,", (unsigned long)i, (unsigned long)d.StateFlags);
            wft_csv_text(f, d.DeviceName); fputc(',', f); wft_csv_text(f, d.DeviceString); fputc(',', f); wft_csv_text(f, d.DeviceID); fputs("\r\n", f);
        }
        fclose(f);
    }
    /* GPU driver version/date come from the display device class key; essential context for TDR/LiveKernelEvent. */
    f = open_in(dir, L"display_drivers.csv"); if (!f) return;
    fputs("instance,description,provider,driver_version,driver_date,vram_MB,matching_device_id\r\n", f);
    const WCHAR *cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            WCHAR sub[32]; DWORD n = 32;
            if (RegEnumKeyExW(k, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (n != 4 || sub[0] < L'0' || sub[0] > L'9') continue;          /* "0000", "0001", not "Configuration"/"Properties" */
            WCHAR path[256], desc[256], prov[128], ver[64], date[64], mid[256];
            wft_wsnprintf(path, 256, L"%ls\\%ls", cls, sub);
            if (!reg_str(HKEY_LOCAL_MACHINE, path, L"DriverDesc", desc, 256)) continue;
            reg_str(HKEY_LOCAL_MACHINE, path, L"ProviderName", prov, 128);
            reg_str(HKEY_LOCAL_MACHINE, path, L"DriverVersion", ver, 64);
            reg_str(HKEY_LOCAL_MACHINE, path, L"DriverDate", date, 64);
            reg_str(HKEY_LOCAL_MACHINE, path, L"MatchingDeviceId", mid, 256);
            ULONGLONG vram = 0; DWORD vs = sizeof vram;
            RegGetValueW(HKEY_LOCAL_MACHINE, path, L"HardwareInformation.qwMemorySize", RRF_RT_REG_QWORD, NULL, &vram, &vs);
            wft_csv_text(f, sub); fputc(',', f); wft_csv_text(f, desc); fputc(',', f); wft_csv_text(f, prov); fputc(',', f);
            wft_csv_text(f, ver); fputc(',', f); wft_csv_text(f, date); fprintf(f, ",%llu,", (unsigned long long)(vram / (1024ull * 1024ull)));
            wft_csv_text(f, cfg->redact ? L"<redacted>" : mid); fputs("\r\n", f);
        }
        RegCloseKey(k);
    }
    fclose(f);
}

/* power.txt */
void sysinfo_write_power(const WCHAR *dir, PowerFacts *pf) {
    pf->max_temp_c = pf->min_passive_pct = WFT_NA;
    FILE *f = open_in(dir, L"power.txt"); if (!f) return;
    fputs("Power, frequency and thermal state\r\n==================================\r\n", f);
    GUID *scheme = NULL;
    if (PowerGetActiveScheme(NULL, &scheme) == ERROR_SUCCESS && scheme) {
        WCHAR name[128] = {0}; DWORD sz = sizeof name;
        if (PowerReadFriendlyName(NULL, scheme, NULL, NULL, (PUCHAR)name, &sz) == ERROR_SUCCESS) { fputs("Active power plan: ", f); wft_write_utf8(f, name); fputs("\r\n", f); }
        LocalFree(scheme);
    }
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1; if (ncpu > 256) ncpu = 256;
    WftProcPowerInfo *ppi = (WftProcPowerInfo *)calloc(ncpu, sizeof *ppi);
    if (ppi && CallNtPowerInformation(ProcessorInformation, NULL, 0, ppi, ncpu * (ULONG)sizeof *ppi) == 0) {
        fputs("\r\nPer-CPU frequency (MHz). MhzLimit below MaxMhz means the CPU is being capped (power plan, thermal, firmware).\r\ncpu,max_mhz,current_mhz,limit_mhz\r\n", f);
        for (DWORD i = 0; i < ncpu; ++i) fprintf(f, "%lu,%lu,%lu,%lu\r\n", (unsigned long)ppi[i].Number, (unsigned long)ppi[i].MaxMhz, (unsigned long)ppi[i].CurrentMhz, (unsigned long)ppi[i].MhzLimit);
    }
    free(ppi);
    double t; char nm[32];
    fputs("\r\nThermal zones (ACPI; many desktops do not expose any)\r\n", f);
    if (pdh_oneshot_extreme(L"\\Thermal Zone Information(*)\\Temperature", true, &t, nm, sizeof nm)) {
        pf->max_temp_c = t - 273.15;                       /* counter is in kelvin */
        fprintf(f, "Hottest zone: %s = %.1f C\r\n", nm, pf->max_temp_c);
    } else fputs("Temperature counters not available.\r\n", f);
    if (pdh_oneshot_extreme(L"\\Thermal Zone Information(*)\\% Passive Limit", false, &t, nm, sizeof nm)) {
        pf->min_passive_pct = t;
        fprintf(f, "Lowest passive limit: %s = %.0f%% (below 100%% means thermal throttling)\r\n", nm, t);
    }
    fclose(f);
}

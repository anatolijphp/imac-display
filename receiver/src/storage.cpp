#include "storage.h"

#include <fcntl.h>
#include <libudev.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <sstream>

#include "protocol.h"

extern char** environ;

namespace {

const std::string kRun = "/run/imacdisplay";
const std::string kVolumes = kRun + "/volumes";
const std::string kSamba = kRun + "/samba";
const std::string kSmbConf = kSamba + "/smb.conf";
const char* kShareUser = "imac";  // system user, see linux/rootfs/etc/sysusers.d/imacdisplay.conf

const std::set<std::string> kReadWrite = {"vfat", "exfat", "ntfs", "ntfs3", "ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs"};
const std::set<std::string> kReadOnly = {"iso9660", "udf", "hfsplus", "hfs"};  // hfsplus: Linux writes to journaled HFS+ are unsafe
const std::set<std::string> kXattrFs = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs"};
const std::set<std::string> kSkipPartTypes = {
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",  // EFI system partition
    "21686148-6449-6e6f-744e-656564454649",  // BIOS boot
    "0xef",                                  // EFI on MBR
};

std::string prop(udev_device* d, const char* k) {
  const char* v = udev_device_get_property_value(d, k);
  return v ? v : "";
}

// udev's *_ENC properties keep the real characters, with \xNN escapes for unsafe ones.
std::string decode_udev(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
      out += char(std::stoi(s.substr(i + 2, 2), nullptr, 16));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string random_password() {
  static const char cs[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::random_device rd;
  std::string s;
  for (int i = 0; i < 24; i++) s += cs[rd() % (sizeof cs - 1)];
  return s;
}

void mkdirs(const std::string& p) {
  for (size_t i = 1; i <= p.size(); i++)
    if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

// Sysfs path of the whole disk that holds the filesystem mounted at `path`.
std::string disk_of_mount(const char* path) {
  struct stat st;
  if (stat(path, &st) < 0) return "";
  char link[64], real[PATH_MAX];
  snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
  if (!realpath(link, real)) return "";
  std::string p = real;
  if (access((p + "/partition").c_str(), F_OK) == 0) p = p.substr(0, p.rfind('/'));
  return p;
}

// Runs a program and waits; returns its exit status (or -1). stdin_data is written to its stdin.
int run(const std::vector<std::string>& argv, const std::string& stdin_data = "") {
  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC) < 0) return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pfd[0], 0);
  std::vector<char*> av;
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(pfd[0]);
  if (rc != 0) { close(pfd[1]); return -1; }
  if (!stdin_data.empty()) (void)!write(pfd[1], stdin_data.data(), stdin_data.size());
  close(pfd[1]);
  int st;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Runs a program and returns its standard output (empty on failure).
std::string capture(const std::vector<std::string>& argv) {
  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC) < 0) return "";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
  std::vector<char*> av;
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(pfd[1]);
  std::string out;
  if (rc == 0) {
    char buf[4096];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof buf)) > 0) out.append(buf, n);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  }
  close(pfd[0]);
  return out;
}

bool is_mountpoint(const std::string& dir) {
  std::ifstream mi("/proc/self/mountinfo");
  std::string line;
  while (std::getline(mi, line)) {
    std::istringstream ls(line);
    std::string a, b, c, d, mp;
    ls >> a >> b >> c >> d >> mp;
    if (mp == dir) return true;
  }
  return false;
}

}  // namespace

Storage::Storage(Options o, Emit emit) : opt_(std::move(o)), emit_(std::move(emit)) {
  root_disk_ = disk_of_mount("/");
  boot_disk_ = disk_of_mount("/boot");
  mkdirs(kVolumes);
  mkdirs(kSamba);
  udev_ = udev_new();
  mon_ = udev_monitor_new_from_netlink(udev_, "udev");
  udev_monitor_filter_add_match_subsystem_devtype(mon_, "block", nullptr);
  udev_monitor_enable_receiving(mon_);

  // Drives that were already plugged in.
  udev_enumerate* en = udev_enumerate_new(udev_);
  udev_enumerate_add_match_subsystem(en, "block");
  udev_enumerate_scan_devices(en);
  udev_list_entry* e;
  udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
    if (udev_device* d = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(e))) {
      consider(d);
      udev_device_unref(d);
    }
  }
  udev_enumerate_unref(en);

  if (opt_.share_macos) mount_macos();
}

// The iMac's own macOS: the APFS Data volume (users' files) on the internal disk, mounted read-only with
// apfs-fuse, which opens the disk O_RDONLY and has no write operations (see linux/apfs-fuse/build.sh).
// Nothing on the internal disk outside the Linux partitions is ever written (CLAUDE.md).
void Storage::mount_macos() {
  udev_enumerate* en = udev_enumerate_new(udev_);
  udev_enumerate_add_match_subsystem(en, "block");
  udev_enumerate_add_match_property(en, "ID_FS_TYPE", "apfs");
  udev_enumerate_scan_devices(en);
  udev_list_entry* e;
  int n = 0;
  udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
    udev_device* d = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(e));
    if (!d) continue;
    udev_device* disk = udev_device_get_parent_with_subsystem_devtype(d, "block", "disk");
    std::string devnode = udev_device_get_devnode(d);
    std::string disk_sys = disk ? udev_device_get_syspath(disk) : "";
    std::string syspath = udev_device_get_syspath(d);
    udev_device_unref(d);
    if (disk_sys != root_disk_) continue;  // internal disk only; USB APFS drives are not supported

    // apfsutil lists the volumes: "Volume N <uuid>", then "Role:", "Name:", "FileVault:" lines.
    std::istringstream out(capture({"apfsutil", devnode}));
    std::string line, vol, role, name, fv;
    auto take = [&]() {
      if (vol.empty() || role != "Data") return;
      if (fv != "No") {
        fprintf(stderr, "storage: macOS volume \"%s\" on %s is FileVault-encrypted, not shared\n", name.c_str(), devnode.c_str());
        return;
      }
      Volume v;
      v.id = n ? "imac-macos-" + std::to_string(n) : "imac-macos";
      v.devnode = devnode;
      v.syspath = syspath;
      v.disk_syspath = disk_sys;
      v.mountpoint = kVolumes + "/" + v.id;
      v.share_path = v.mountpoint + "/root";  // apfs-fuse shows the volume's files under root/
      v.fs = "apfs";
      v.read_only = true;
      v.internal = true;
      std::string nm = name.substr(0, name.find(" (Case"));
      v.label = "iMac macOS (" + nm + ")";
      v.share = share_name(v.label);
      mkdirs(v.mountpoint);
      if (!is_mountpoint(v.mountpoint) &&
          run({"apfs-fuse", "-v", vol, "-o", "ro", devnode, v.mountpoint}) != 0) {
        fprintf(stderr, "storage: apfs-fuse could not mount volume %s of %s\n", vol.c_str(), devnode.c_str());
        rmdir(v.mountpoint.c_str());
        return;
      }
      struct statvfs sv;
      if (statvfs(v.mountpoint.c_str(), &sv) == 0) v.size = uint64_t(sv.f_blocks) * sv.f_frsize;
      fprintf(stderr, "storage: mounted macOS volume %s \"%s\" of %s read-only at %s\n", vol.c_str(), nm.c_str(),
              devnode.c_str(), v.mountpoint.c_str());
      vols_[v.id] = v;
      n++;
    };
    while (std::getline(out, line)) {
      if (line.rfind("Volume ", 0) == 0) {
        take();
        std::istringstream ls(line.substr(7));
        ls >> vol;
        role = name = fv = "";
      } else if (line.rfind("Role:", 0) == 0) {
        role = line.substr(line.find_first_not_of(' ', 5));
      } else if (line.rfind("Name:", 0) == 0) {
        name = line.substr(line.find_first_not_of(' ', 5));
      } else if (line.rfind("FileVault:", 0) == 0) {
        fv = line.substr(line.find_first_not_of(' ', 10));
      }
    }
    take();
  }
  udev_enumerate_unref(en);
}

Storage::~Storage() {
  stop_samba();
  for (auto& [id, v] : vols_) unmount_volume(v, false);
  udev_monitor_unref(mon_);
  udev_unref(udev_);
}

int Storage::udev_fd() const { return udev_monitor_get_fd(mon_); }

void Storage::on_udev() {
  while (udev_device* d = udev_monitor_receive_device(mon_)) {
    std::string action = udev_device_get_action(d) ? udev_device_get_action(d) : "";
    if (action == "remove") removed(d);
    else if (action == "add" || action == "change") consider(d);
    udev_device_unref(d);
  }
}

void Storage::consider(udev_device* d) {
  if (prop(d, "ID_BUS") != "usb") return;  // USB drives only, never the internal drive
  if (prop(d, "ID_FS_USAGE") != "filesystem") return;
  std::string devtype = prop(d, "DEVTYPE");
  if (devtype == "disk" && !prop(d, "ID_PART_TABLE_TYPE").empty()) return;  // use its partitions instead
  if (kSkipPartTypes.count(prop(d, "ID_PART_ENTRY_TYPE"))) return;
  std::string fs = prop(d, "ID_FS_TYPE");
  if (!kReadWrite.count(fs) && !kReadOnly.count(fs)) return;

  udev_device* disk = devtype == "disk" ? d : udev_device_get_parent_with_subsystem_devtype(d, "block", "disk");
  std::string disk_sys = disk ? udev_device_get_syspath(disk) : "";
  if (disk_sys.empty() || disk_sys == root_disk_ || disk_sys == boot_disk_) return;  // belt and braces

  Volume v;
  v.devnode = udev_device_get_devnode(d);
  v.syspath = udev_device_get_syspath(d);
  v.disk_syspath = disk_sys;
  for (auto& [id, o] : vols_)
    if (o.syspath == v.syspath) return;  // already mounted

  std::string uuid = prop(d, "ID_FS_UUID");
  std::string id;
  for (char c : uuid.empty() ? std::string(udev_device_get_sysname(d)) : uuid)
    if (isalnum((unsigned char)c)) id += tolower(c);
  if (id.size() > 16) id.resize(16);
  if (vols_.count(id)) id += "-" + std::string(udev_device_get_sysname(d));
  v.id = id;
  v.fs = fs;
  v.label = decode_udev(prop(d, "ID_FS_LABEL_ENC"));
  if (v.label.empty()) v.label = prop(d, "ID_FS_LABEL");
  if (v.label.empty()) v.label = "USB drive";
  std::string fs_size = prop(d, "ID_FS_SIZE");
  if (!fs_size.empty()) v.size = strtoull(fs_size.c_str(), nullptr, 10);
  else if (const char* s = udev_device_get_sysattr_value(d, "size")) v.size = strtoull(s, nullptr, 10) * 512;
  v.read_only = opt_.read_only || kReadOnly.count(fs);
  v.share = share_name(v.label);  // Finder shows the share name as the volume name
  if (udev_device* usb = udev_device_get_parent_with_subsystem_devtype(d, "usb", "usb_device"))
    v.usb_syspath = udev_device_get_syspath(usb);
  v.mountpoint = kVolumes + "/" + id;
  v.share_path = v.mountpoint;

  if (!mount_volume(v)) return;
  vols_[v.id] = v;
  if (session_) {
    write_samba_config();
    reload_samba();
    announce(v);
  }
}

bool Storage::mount_volume(Volume& v) {
  mkdirs(v.mountpoint);
  unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME;
  std::string fstype = v.fs == "ntfs" ? "ntfs3" : v.fs;
  std::string data;
  if (fstype == "vfat") data = "utf8,shortname=mixed";
  if (fstype == "ntfs3") data = "iocharset=utf8";
  if (v.read_only) {
    flags |= MS_RDONLY;
    if (fstype == "ext3" || fstype == "ext4") data = "noload";  // do not replay the journal: no writes at all
    if (fstype == "xfs") data = "norecovery";
  }
  if (mount(v.devnode.c_str(), v.mountpoint.c_str(), fstype.c_str(), flags, data.empty() ? nullptr : data.c_str()) < 0) {
    int e = errno;
    if (!v.read_only && (e == EROFS || e == EACCES)) {
      // Write-protected switch, or a filesystem the kernel will only mount read-only (dirty NTFS): retry read-only.
      v.read_only = true;
      return mount_volume(v);
    }
    fprintf(stderr, "storage: mount %s (%s) failed: %s\n", v.devnode.c_str(), fstype.c_str(), strerror(e));
    rmdir(v.mountpoint.c_str());
    return false;
  }
  fprintf(stderr, "storage: mounted %s \"%s\" (%s, %.1f GB, %s) at %s\n", v.devnode.c_str(), v.label.c_str(),
          v.fs.c_str(), v.size / 1e9, v.read_only ? "read-only" : "read-write", v.mountpoint.c_str());
  return true;
}

void Storage::unmount_volume(Volume& v, bool lazy) {
  sync();
  if (umount2(v.mountpoint.c_str(), lazy ? MNT_DETACH : 0) < 0) {
    fprintf(stderr, "storage: umount %s: %s, detaching\n", v.mountpoint.c_str(), strerror(errno));
    umount2(v.mountpoint.c_str(), MNT_DETACH);
  }
  rmdir(v.mountpoint.c_str());
  fprintf(stderr, "storage: unmounted %s \"%s\"\n", v.devnode.c_str(), v.label.c_str());
}

void Storage::removed(udev_device* d) {
  std::string sys = udev_device_get_syspath(d);
  for (auto it = vols_.begin(); it != vols_.end();) {
    if (it->second.syspath == sys || it->second.disk_syspath == sys) {
      fprintf(stderr, "storage: %s pulled out\n", it->second.devnode.c_str());
      unmount_volume(it->second, true);
      if (session_) emit_(proto::STORAGE_DETACHED, nlohmann::json{{"id", it->first}}.dump());
      it = vols_.erase(it);
      if (session_) { write_samba_config(); reload_samba(); }
    } else {
      ++it;
    }
  }
}

std::string Storage::share_name(const std::string& label) const {
  // Letters, digits, space and . _ - ( ) are kept (UTF-8 letters too); anything else becomes '_'.
  std::string n;
  for (unsigned char c : label)
    n += (isalnum(c) || c >= 0x80 || strchr(" ._-()", c)) ? char(c) : '_';
  while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
  while (!n.empty() && n.front() == ' ') n.erase(0, 1);
  if (n.empty()) n = "USB drive";
  if (n.size() > 60) n.resize(60);
  // Samba reserves these section names.
  std::string lower;
  for (char c : n) lower += char(tolower((unsigned char)c));
  if (lower == "global" || lower == "homes" || lower == "printers") n += " drive";
  auto taken = [&](const std::string& x) {
    for (auto& [id, v] : vols_) {
      std::string a = v.share, b = x;
      for (auto& c : a) c = char(tolower((unsigned char)c));
      for (auto& c : b) c = char(tolower((unsigned char)c));
      if (a == b) return true;
    }
    return false;
  };
  std::string u = n;
  for (int i = 2; taken(u); i++) u = n + " " + std::to_string(i);
  return u;
}

void Storage::announce(const Volume& v) {
  emit_(proto::STORAGE_ATTACHED, nlohmann::json{{"id", v.id},
                                                {"label", v.label},
                                                {"fs", v.fs},
                                                {"size", v.size},
                                                {"read_only", v.read_only},
                                                {"share", v.share},
                                                {"user", kShareUser},
                                                {"password", password_}}
                                     .dump());
}

void Storage::eject(const std::string& id) {
  auto it = vols_.find(id);
  if (it == vols_.end()) return;
  if (it->second.internal) {
    // The MacBook has unmounted the iMac's macOS volume. It stays mounted here (read-only) and is
    // offered again in the next session. The internal disk is never unmounted or powered off.
    emit_(proto::STORAGE_DETACHED, nlohmann::json{{"id", id}}.dump());
    return;
  }
  Volume v = it->second;
  vols_.erase(it);
  write_samba_config();
  reload_samba();
  unmount_volume(v, false);
  if (power_off_disk_if_unused(v.disk_syspath) && !v.usb_syspath.empty()) ejected_usb_.push_back(v.usb_syspath);
  emit_(proto::STORAGE_DETACHED, nlohmann::json{{"id", id}}.dump());
}

bool Storage::power_off_disk_if_unused(const std::string& disk_syspath) {
  if (disk_syspath == root_disk_ || disk_syspath == boot_disk_) return false;  // never the internal disk
  for (auto& [id, v] : vols_)
    if (v.disk_syspath == disk_syspath) return false;
  // Only power off if none of its partitions is mounted by anyone else either.
  std::ifstream mounts("/proc/self/mountinfo");
  std::string line;
  std::string dev = disk_syspath.substr(disk_syspath.rfind('/') + 1);  // e.g. sdb
  while (std::getline(mounts, line))
    if (line.find(" /dev/" + dev) != std::string::npos) {
      fprintf(stderr, "storage: %s still has mounted partitions, not powering it off\n", dev.c_str());
      return false;
    }
  std::ofstream del(disk_syspath + "/device/delete");
  if (!del) return false;
  del << "1\n";
  fprintf(stderr, "storage: %s powered off, safe to remove\n", dev.c_str());
  return true;
}

void Storage::write_samba_config() {
  std::ofstream f(kSmbConf + ".new");
  f << "# Written by imacdisplayd. Shares USB drives with the MacBook over " << opt_.iface << " only.\n"
    << "[global]\n"
    << "  server role = standalone server\n"
    << "  interfaces = " << opt_.iface << "\n"
    << "  bind interfaces only = yes\n"
    << "  smb ports = 445\n"
    << "  disable netbios = yes\n"
    << "  server min protocol = SMB3\n"
    << "  map to guest = never\n"
    << "  passdb backend = tdbsam:" << kSamba << "/passdb.tdb\n"
    << "  private dir = " << kSamba << "/private\n"
    << "  lock directory = " << kSamba << "/lock\n"
    << "  state directory = " << kSamba << "/state\n"
    << "  cache directory = " << kSamba << "/cache\n"
    << "  pid directory = " << kSamba << "\n"
    << "  ncalrpc dir = " << kSamba << "/ncalrpc\n"
    << "  log file = " << kSamba << "/log.%m\n"
    << "  max log size = 1000\n"
    << "  load printers = no\n"
    << "  printing = bsd\n"
    << "  printcap name = /dev/null\n"
    << "  disable spoolss = yes\n"
    << "  fruit:model = iMac\n";
  for (auto& [id, v] : vols_) {
    if (v.internal && wireless_) continue;  // the iMac's macOS: over the cable only
    f << "\n[" << v.share << "]\n"
      << "  comment = " << v.label << "\n"
      << "  path = " << v.share_path << "\n"
      << "  valid users = " << kShareUser << "\n"
      << "  force user = root\n"
      << "  read only = " << (v.read_only ? "yes" : "no") << "\n";
    // Mac metadata in extended attributes where the filesystem has them; FAT, exFAT and NTFS
    // get macOS's own ._ files instead.
    if (kXattrFs.count(v.fs))
      f << "  vfs objects = catia fruit streams_xattr\n  fruit:metadata = stream\n";
    else
      f << "  vfs objects = catia\n";
  }
  f.close();
  rename((kSmbConf + ".new").c_str(), kSmbConf.c_str());
}

void Storage::start_samba() {
  if (smbd_ > 0) return;
  for (const char* d : {"private", "lock", "state", "cache", "ncalrpc"}) mkdirs(kSamba + "/" + d);
  chmod((kSamba + "/private").c_str(), 0700);
  write_samba_config();
  password_ = random_password();
  // A fresh password for this session; the passdb lives in /run and is gone after a reboot.
  if (run({"smbpasswd", "-c", kSmbConf, "-s", "-a", kShareUser}, password_ + "\n" + password_ + "\n") != 0)
    fprintf(stderr, "storage: smbpasswd failed (does the system user '%s' exist?)\n", kShareUser);
  std::vector<std::string> argv = {"smbd", "--foreground", "--no-process-group", "-s", kSmbConf};
  std::vector<char*> av;
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  // Own process group: when smbd shuts down it signals its whole process group, which would otherwise
  // include the receiver (and end it with every session).
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  int rc = posix_spawnp(&smbd_, "smbd", nullptr, &attr, av.data(), environ);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    fprintf(stderr, "storage: cannot start smbd\n");
    smbd_ = -1;
    return;
  }
  fprintf(stderr, "storage: smbd started on %s (pid %d)\n", opt_.iface.c_str(), smbd_);
}

void Storage::stop_samba() {
  if (smbd_ <= 0) return;
  kill(smbd_, SIGTERM);
  int st;
  waitpid(smbd_, &st, 0);
  smbd_ = -1;
  fprintf(stderr, "storage: smbd stopped\n");
}

void Storage::reload_samba() {
  if (smbd_ > 0) kill(smbd_, SIGHUP);
}

void Storage::session_started() {
  session_ = true;
  // Drives ejected in an earlier session but never unplugged: switch their USB device off and on again,
  // so they are found again (an ejected drive would otherwise stay invisible until it is replugged).
  for (const auto& usb : ejected_usb_) {
    std::ofstream a0(usb + "/authorized");
    if (!a0) continue;  // unplugged meanwhile
    a0 << "0\n";
    a0.close();
    std::ofstream a1(usb + "/authorized");
    a1 << "1\n";
    fprintf(stderr, "storage: bringing back the drive ejected earlier at %s\n", usb.c_str());
  }
  ejected_usb_.clear();
  start_samba();
  for (auto& [id, v] : vols_)
    if (!(v.internal && wireless_)) announce(v);
}

void Storage::session_ended() {
  session_ = false;
  stop_samba();
}

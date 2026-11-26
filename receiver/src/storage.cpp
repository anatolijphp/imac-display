#include "storage.h"

#include <fcntl.h>
#include <libudev.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
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
  v.share = "usb-" + id;
  v.fs = fs;
  v.label = decode_udev(prop(d, "ID_FS_LABEL_ENC"));
  if (v.label.empty()) v.label = prop(d, "ID_FS_LABEL");
  if (v.label.empty()) v.label = "USB drive";
  std::string fs_size = prop(d, "ID_FS_SIZE");
  if (!fs_size.empty()) v.size = strtoull(fs_size.c_str(), nullptr, 10);
  else if (const char* s = udev_device_get_sysattr_value(d, "size")) v.size = strtoull(s, nullptr, 10) * 512;
  v.read_only = opt_.read_only || kReadOnly.count(fs);
  v.mountpoint = kVolumes + "/" + id;

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
  Volume v = it->second;
  vols_.erase(it);
  write_samba_config();
  reload_samba();
  unmount_volume(v, false);
  power_off_disk_if_unused(v.disk_syspath);
  emit_(proto::STORAGE_DETACHED, nlohmann::json{{"id", id}}.dump());
}

void Storage::power_off_disk_if_unused(const std::string& disk_syspath) {
  for (auto& [id, v] : vols_)
    if (v.disk_syspath == disk_syspath) return;
  // Only power off if none of its partitions is mounted by anyone else either.
  std::ifstream mounts("/proc/self/mountinfo");
  std::string line;
  std::string dev = disk_syspath.substr(disk_syspath.rfind('/') + 1);  // e.g. sdb
  while (std::getline(mounts, line))
    if (line.find(" /dev/" + dev) != std::string::npos) {
      fprintf(stderr, "storage: %s still has mounted partitions, not powering it off\n", dev.c_str());
      return;
    }
  std::ofstream del(disk_syspath + "/device/delete");
  if (del) {
    del << "1\n";
    fprintf(stderr, "storage: %s powered off, safe to remove\n", dev.c_str());
  }
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
    f << "\n[" << v.share << "]\n"
      << "  comment = " << v.label << "\n"
      << "  path = " << v.mountpoint << "\n"
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
  if (posix_spawnp(&smbd_, "smbd", nullptr, nullptr, av.data(), environ) != 0) {
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
  start_samba();
  for (auto& [id, v] : vols_) announce(v);
}

void Storage::session_ended() {
  session_ = false;
  stop_samba();
}

// USB drives plugged into the iMac, shared with the MacBook over SMB (PROTOCOL.md, Storage).
#pragma once
#include <sys/types.h>

#include <cstdint>

#include <functional>
#include <map>
#include <string>
#include <vector>

struct udev;
struct udev_monitor;
struct udev_device;

class Storage {
 public:
  struct Options {
    std::string iface;        // Samba listens on this interface only
    bool read_only = false;   // mount everything read-only (testing)
    bool share_macos = true;  // offer the iMac's own macOS Data volume, read-only (apfs-fuse)
  };
  // emit(type, json) sends one STORAGE_* message on ctrl.
  using Emit = std::function<void(uint8_t type, const std::string& json)>;
  Storage(Options o, Emit emit);
  ~Storage();

  int udev_fd() const;
  void on_udev();

  void session_started();               // starts Samba, announces mounted volumes
  void session_ended();                 // stops Samba; drives stay mounted
  void eject(const std::string& id);    // STORAGE_EJECT from the MacBook

 private:
  struct Volume {
    std::string id, devnode, syspath, disk_syspath, label, fs, mountpoint, share;
    std::string share_path;  // directory shared over SMB; usually the mountpoint
    std::string usb_syspath; // the USB device the disk sits on (to bring it back after an eject)
    uint64_t size = 0;
    bool read_only = false;
    bool internal = false;   // the iMac's own macOS volume: never unmounted on eject, never powered off
  };
  void mount_macos();
  void consider(udev_device* d);
  void removed(udev_device* d);
  bool mount_volume(Volume& v);
  void unmount_volume(Volume& v, bool lazy);
  void announce(const Volume& v);
  void write_samba_config();
  void start_samba();
  void stop_samba();
  void reload_samba();
  bool power_off_disk_if_unused(const std::string& disk_syspath);
  std::string share_name(const std::string& label) const;  // readable, SMB-safe, unique

  Options opt_;
  Emit emit_;
  udev* udev_ = nullptr;
  udev_monitor* mon_ = nullptr;
  std::map<std::string, Volume> vols_;  // by id
  bool session_ = false;
  std::string password_;
  std::vector<std::string> ejected_usb_;  // powered off by an eject; brought back at the next session
  pid_t smbd_ = -1;
  std::string root_disk_, boot_disk_;  // sysfs paths of the internal disk(s) holding / and /boot: never touched
};

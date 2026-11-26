// Wi-Fi networks for the waiting screen's Wi-Fi panel: iwd (the Wi-Fi daemon on the iMac) over D-Bus.
// Lists the networks in range, joins one (asking for its password through an iwd agent), and reports
// what happened. Everything runs on the caller's thread: poll fd() and call process().
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;
typedef struct sd_bus_error sd_bus_error;

struct WifiNet {
  std::string path, name, type;  // type: "open", "psk", "8021x", "wep"
  int bars = 0;                  // 0..4 from the signal strength
  bool known = false, connected = false;
};

class Wifi {
 public:
  // changed() is called after the list or the status changed.
  Wifi(std::string iface, std::function<void()> changed);
  ~Wifi();
  bool ok() const { return bus_ != nullptr; }
  int fd() const;
  short events() const;
  void process();

  void set_iface(const std::string& iface) { iface_ = iface; refresh(); }
  void scan();     // ask iwd to look for networks (the list updates when it is done)
  void refresh();  // read the list again
  // Join a network. passphrase: for a secured network iwd does not know yet (or whose password
  // changed); empty for open and known networks.
  void connect(const std::string& path, const std::string& passphrase);

  const std::vector<WifiNet>& networks() const { return nets_; }
  bool scanning() const { return scanning_; }
  std::string connected_name() const;
  std::string status() const { return status_; }  // "Connecting to …", errors; empty otherwise
  // Set when iwd asked for a password we did not have (a new secured network, or a wrong password):
  // the network's path, cleared by connect().
  std::string password_needed() const { return password_needed_; }

 private:
  static const struct sd_bus_vtable* agent_vtable();
  static int on_objects(sd_bus_message* m, void* self, sd_bus_error* e);
  static int on_ordered(sd_bus_message* m, void* self, sd_bus_error* e);
  static int on_connected(sd_bus_message* m, void* self, sd_bus_error* e);
  static int on_signal(sd_bus_message* m, void* self, sd_bus_error* e);
  static int agent_passphrase(sd_bus_message* m, void* self, sd_bus_error* e);
  static int agent_cancel(sd_bus_message* m, void* self, sd_bus_error* e);
  static int agent_release(sd_bus_message* m, void* self, sd_bus_error* e);
  static int agent_refuse(sd_bus_message* m, void* self, sd_bus_error* e);
  std::string name_of(const std::string& path) const;

  std::string iface_;
  std::function<void()> changed_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* objects_call_ = nullptr;
  bool refresh_again_ = false;
  std::string station_;  // iwd object path of the station on iface_

  struct Props { std::string name, type, device; bool known = false, connected = false; };
  std::map<std::string, Props> props_;  // network path -> properties, from the last full read
  std::vector<WifiNet> nets_;
  bool scanning_ = false;
  std::string status_;
  std::string connecting_, passphrase_, password_needed_;
  int reconfigure_pid_ = 0;  // networkctl after a join, collected in process()
};

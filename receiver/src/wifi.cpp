#include "wifi.h"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

const char* kIwd = "net.connman.iwd";
const char* kAgentPath = "/imacdisplay/wifi_agent";

// One property value (the variant after its key): strings, object paths and booleans are kept, the
// rest is skipped.
int read_variant(sd_bus_message* m, std::string* s, bool* b) {
  char type;
  const char* contents = nullptr;
  int r = sd_bus_message_peek_type(m, &type, &contents);
  if (r <= 0) return r;
  if (!contents || (strcmp(contents, "s") && strcmp(contents, "o") && strcmp(contents, "b")))
    return sd_bus_message_skip(m, "v");
  if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0) return r;
  if (contents[0] == 'b') {
    int v = 0;
    r = sd_bus_message_read_basic(m, 'b', &v);
    if (b) *b = v;
  } else {
    const char* v = nullptr;
    r = sd_bus_message_read_basic(m, contents[0], &v);
    if (s && v) *s = v;
  }
  if (r < 0) return r;
  return sd_bus_message_exit_container(m);
}

std::string error_text(const sd_bus_error* e) {
  std::string n = e && e->name ? e->name : "";
  if (n == "net.connman.iwd.Failed" || n == "net.connman.iwd.InvalidFormat") return "Could not join. Check the password.";
  if (n == "net.connman.iwd.NotFound") return "That network is out of range.";
  if (n == "net.connman.iwd.NotSupported") return "This kind of network is not supported here.";
  if (n == "net.connman.iwd.Timeout" || n == "org.freedesktop.DBus.Error.NoReply") return "No answer from the network.";
  return std::string("Could not join") + (e && e->message ? std::string(": ") + e->message : ".");
}

}  // namespace

const sd_bus_vtable* Wifi::agent_vtable() {
  static const sd_bus_vtable vt[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_METHOD("Release", "", "", agent_release, 0),
      SD_BUS_METHOD("RequestPassphrase", "o", "s", agent_passphrase, 0),
      SD_BUS_METHOD("RequestPrivateKeyPassphrase", "o", "s", agent_refuse, 0),
      SD_BUS_METHOD("RequestUserNameAndPassword", "o", "ss", agent_refuse, 0),
      SD_BUS_METHOD("RequestUserPassword", "os", "s", agent_refuse, 0),
      SD_BUS_METHOD("Cancel", "s", "", agent_cancel, 0),
      SD_BUS_VTABLE_END,
  };
  return vt;
}

Wifi::Wifi(std::string iface, std::function<void()> changed) : iface_(std::move(iface)), changed_(std::move(changed)) {
  if (sd_bus_open_system(&bus_) < 0) {
    bus_ = nullptr;
    fprintf(stderr, "wifi: no system bus; the Wi-Fi panel stays empty\n");
    return;
  }
  // Answer iwd's password questions (only ever for a join started here).
  sd_bus_add_object_vtable(bus_, nullptr, kAgentPath, "net.connman.iwd.Agent", agent_vtable(), this);
  sd_bus_call_method_async(bus_, nullptr, kIwd, "/net/connman/iwd", "net.connman.iwd.AgentManager", "RegisterAgent",
                           nullptr, nullptr, "o", kAgentPath);
  // Any property change in iwd (scan finished, connected, new network): read the list again.
  sd_bus_match_signal(bus_, nullptr, kIwd, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                      on_signal, this);
  sd_bus_match_signal(bus_, nullptr, kIwd, nullptr, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                      on_signal, this);
  sd_bus_match_signal(bus_, nullptr, kIwd, nullptr, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                      on_signal, this);
  refresh();
}

Wifi::~Wifi() {
  if (!bus_) return;
  sd_bus_call_method(bus_, kIwd, "/net/connman/iwd", "net.connman.iwd.AgentManager", "UnregisterAgent", nullptr,
                     nullptr, "o", kAgentPath);
  sd_bus_flush_close_unref(bus_);
}

int Wifi::fd() const { return bus_ ? sd_bus_get_fd(bus_) : -1; }

short Wifi::events() const { return bus_ ? short(sd_bus_get_events(bus_)) : 0; }

void Wifi::process() {
  if (!bus_) return;
  while (sd_bus_process(bus_, nullptr) > 0) {}
}

void Wifi::refresh() {
  if (!bus_) return;
  if (objects_call_) { refresh_again_ = true; return; }  // one read at a time
  sd_bus_call_method_async(bus_, &objects_call_, kIwd, "/", "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", on_objects, this, "");
}

void Wifi::scan() {
  if (!bus_ || station_.empty()) { refresh(); return; }
  sd_bus_call_method_async(bus_, nullptr, kIwd, station_.c_str(), "net.connman.iwd.Station", "Scan", nullptr,
                           nullptr, "");  // "busy" when iwd is scanning anyway: fine
}

void Wifi::connect(const std::string& path, const std::string& passphrase) {
  if (!bus_) return;
  connecting_ = path;
  passphrase_ = passphrase;
  password_needed_.clear();
  status_ = "Connecting to “" + name_of(path) + "”…";
  fprintf(stderr, "wifi: joining \"%s\"\n", name_of(path).c_str());  // never the password
  sd_bus_call_method_async(bus_, nullptr, kIwd, path.c_str(), "net.connman.iwd.Network", "Connect", on_connected,
                           this, "");
  if (changed_) changed_();
}

std::string Wifi::connected_name() const {
  for (auto& n : nets_)
    if (n.connected) return n.name;
  return "";
}

std::string Wifi::name_of(const std::string& path) const {
  auto it = props_.find(path);
  return it == props_.end() ? std::string("?") : it->second.name;
}

// GetManagedObjects: a{oa{sa{sv}}}. Keeps the station for iface_ and every network's properties.
int Wifi::on_objects(sd_bus_message* m, void* self_, sd_bus_error*) {
  auto* self = static_cast<Wifi*>(self_);
  self->objects_call_ = sd_bus_slot_unref(self->objects_call_);
  if (sd_bus_message_is_method_error(m, nullptr)) {
    fprintf(stderr, "wifi: iwd is not answering (%s)\n", sd_bus_message_get_error(m)->name);
    return 0;
  }
  std::map<std::string, std::string> device_names;  // device path -> interface name
  std::map<std::string, bool> station_scanning;      // station (= device) path -> scanning
  std::map<std::string, Props> nets;
  if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") < 0) return 0;
  while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
    const char* path = nullptr;
    sd_bus_message_read_basic(m, 'o', &path);
    std::string p = path ? path : "";
    sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
    while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
      const char* iface = nullptr;
      sd_bus_message_read_basic(m, 's', &iface);
      std::string i = iface ? iface : "";
      bool want = i == "net.connman.iwd.Device" || i == "net.connman.iwd.Station" || i == "net.connman.iwd.Network";
      if (!want) {
        sd_bus_message_skip(m, "a{sv}");
      } else {
        sd_bus_message_enter_container(m, 'a', "{sv}");
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
          const char* key = nullptr;
          sd_bus_message_read_basic(m, 's', &key);
          std::string k = key ? key : "", sv;
          bool bv = false;
          read_variant(m, &sv, &bv);
          if (i == "net.connman.iwd.Device" && k == "Name") device_names[p] = sv;
          else if (i == "net.connman.iwd.Station" && k == "Scanning") station_scanning[p] = bv;
          else if (i == "net.connman.iwd.Network") {
            Props& n = nets[p];
            if (k == "Name") n.name = sv;
            else if (k == "Type") n.type = sv;
            else if (k == "Connected") n.connected = bv;
            else if (k == "KnownNetwork") n.known = !sv.empty();
            else if (k == "Device") n.device = sv;
          }
          sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
      }
      sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    sd_bus_message_exit_container(m);
  }
  sd_bus_message_exit_container(m);

  self->station_.clear();
  for (auto& [path, name] : device_names)
    if (name == self->iface_ && station_scanning.count(path)) self->station_ = path;
  self->scanning_ = self->station_.size() && station_scanning[self->station_];
  self->props_ = std::move(nets);
  if (!self->station_.empty())
    sd_bus_call_method_async(self->bus_, nullptr, kIwd, self->station_.c_str(), "net.connman.iwd.Station",
                             "GetOrderedNetworks", on_ordered, self, "");
  else {
    self->nets_.clear();
    if (self->changed_) self->changed_();
  }
  if (self->refresh_again_) { self->refresh_again_ = false; self->refresh(); }
  return 0;
}

// GetOrderedNetworks: a(on), strongest first, signal in 100 * dBm.
int Wifi::on_ordered(sd_bus_message* m, void* self_, sd_bus_error*) {
  auto* self = static_cast<Wifi*>(self_);
  if (sd_bus_message_is_method_error(m, nullptr)) return 0;
  std::vector<WifiNet> list;
  if (sd_bus_message_enter_container(m, 'a', "(on)") < 0) return 0;
  const char* path = nullptr;
  int16_t sig = 0;
  while (sd_bus_message_read(m, "(on)", &path, &sig) > 0) {
    auto it = self->props_.find(path);
    if (it == self->props_.end() || it->second.name.empty()) continue;
    WifiNet n;
    n.path = path;
    n.name = it->second.name;
    n.type = it->second.type;
    n.known = it->second.known;
    n.connected = it->second.connected;
    n.bars = sig > -6000 ? 4 : sig > -6700 ? 3 : sig > -7500 ? 2 : 1;
    list.push_back(n);
  }
  self->nets_ = std::move(list);
  if (self->changed_) self->changed_();
  return 0;
}

int Wifi::on_connected(sd_bus_message* m, void* self_, sd_bus_error*) {
  auto* self = static_cast<Wifi*>(self_);
  std::string name = self->name_of(self->connecting_);
  if (sd_bus_message_is_method_error(m, nullptr)) {
    const sd_bus_error* e = sd_bus_message_get_error(m);
    std::string en = e && e->name ? e->name : "";
    fprintf(stderr, "wifi: joining \"%s\" failed: %s\n", name.c_str(), en.c_str());
    if (en == "net.connman.iwd.InProgress") return 0;  // an earlier attempt is still running
    if (!self->password_needed_.empty() && (en == "net.connman.iwd.Aborted" || en == "net.connman.iwd.Failed"))
      self->status_ = self->passphrase_.empty() ? "" : "Wrong password, or the network refused it.";
    else
      self->status_ = error_text(e);
    // A password we sent and that was refused: ask again.
    if (!self->passphrase_.empty() && en == "net.connman.iwd.Failed") self->password_needed_ = self->connecting_;
  } else {
    fprintf(stderr, "wifi: joined \"%s\"\n", name.c_str());
    self->status_.clear();
    self->password_needed_.clear();
  }
  self->passphrase_.clear();
  self->connecting_.clear();
  self->refresh();
  if (self->changed_) self->changed_();
  return 0;
}

int Wifi::on_signal(sd_bus_message*, void* self_, sd_bus_error*) {
  static_cast<Wifi*>(self_)->refresh();
  return 0;
}

int Wifi::agent_passphrase(sd_bus_message* m, void* self_, sd_bus_error* e) {
  auto* self = static_cast<Wifi*>(self_);
  const char* path = nullptr;
  sd_bus_message_read(m, "o", &path);
  std::string p = path ? path : "";
  if (p == self->connecting_ && !self->passphrase_.empty())
    return sd_bus_reply_method_return(m, "s", self->passphrase_.c_str());
  // No password typed for it (new network, or iwd's saved one is wrong): the panel asks for one.
  self->password_needed_ = p;
  if (self->changed_) self->changed_();
  return sd_bus_error_set(e, "net.connman.iwd.Agent.Error.Canceled", "password needed");
}

int Wifi::agent_cancel(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }
int Wifi::agent_release(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }
int Wifi::agent_refuse(sd_bus_message*, void*, sd_bus_error* e) {
  return sd_bus_error_set(e, "net.connman.iwd.Agent.Error.Canceled", "not supported on the iMac display");
}

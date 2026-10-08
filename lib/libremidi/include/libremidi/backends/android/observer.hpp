#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/observer.hpp>

NAMESPACE_LIBREMIDI
{
namespace android
{
class observer final
    : public libremidi::observer_api
    , public error_handler
{
public:
  explicit observer(
      libremidi::observer_configuration&& conf, libremidi::android::observer_configuration aconf)
      : configuration{std::move(conf)}
  {
    if (!context::client_name.empty() && context::client_name != aconf.client_name)
    {
      LOGW("Android backend only supports one client name per process");
    }
    context::client_name = std::string(aconf.client_name);

    if (configuration.has_callbacks())
    {
      sink = std::make_shared<observer_sink>();
      sink->input_added = configuration.input_added;
      sink->input_removed = configuration.input_removed;
      sink->output_added = configuration.output_added;
      sink->output_removed = configuration.output_removed;
      context::register_observer(sink);

      auto env = context::get_thread_env();
      auto ctx = env ? context::get_context(env) : nullptr;
      if (ctx)
      {
        const auto devices = context::list_devices(env, ctx);
        {
          std::lock_guard l{context::registry_mutex};
          context::hotplug_snapshot = devices;
        }
        if (configuration.notify_in_constructor)
          notify_existing(devices);
      }
    }
  }

  ~observer()
  {
    if (sink)
      context::unregister_observer(sink);

    auto env = context::get_thread_env();
    if (env)
    {
      context::cleanup_devices(env);
    }
  }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  std::vector<libremidi::input_port> get_input_ports() const noexcept override
  {
    auto env = context::get_thread_env();
    if (!env)
      return {};

    auto ctx = context::get_context(env);
    if (!ctx)
      return {};

    context::refresh_midi_devices(env, ctx, false);

    std::vector<libremidi::input_port> ports;
    for (size_t i = 0; i < context::midi_devices.size(); ++i)
    {
      libremidi::input_port port;
      port.api = libremidi::API::ANDROID_AMIDI;
      port.port_name = context::port_name(env, i);
      port.port = i;
      if (i < context::midi_device_ids.size())
        port.device = static_cast<std::uint64_t>(context::midi_device_ids[i]);
      ports.push_back(std::move(port));
    }

    return ports;
  }

  std::vector<libremidi::output_port> get_output_ports() const noexcept override
  {
    auto env = context::get_thread_env();
    if (!env)
      return {};

    auto ctx = context::get_context(env);
    if (!ctx)
      return {};

    context::refresh_midi_devices(env, ctx, true);

    std::vector<libremidi::output_port> ports;
    for (size_t i = 0; i < context::midi_devices.size(); ++i)
    {
      libremidi::output_port port;
      port.api = libremidi::API::ANDROID_AMIDI;
      port.port_name = context::port_name(env, i);
      port.port = i;
      if (i < context::midi_device_ids.size())
        port.device = static_cast<std::uint64_t>(context::midi_device_ids[i]);
      ports.push_back(std::move(port));
    }

    return ports;
  }

private:
  void notify_existing(const std::vector<device_event>& devices)
  {
    std::size_t in_index = 0, out_index = 0;
    for (const auto& d : devices)
    {
      if (d.output_ports > 0)
      {
        if (configuration.input_added)
          configuration.input_added(context::make_port<libremidi::input_port>(d, in_index));
        in_index++;
      }
      if (d.input_ports > 0)
      {
        if (configuration.output_added)
          configuration.output_added(context::make_port<libremidi::output_port>(d, out_index));
        out_index++;
      }
    }
  }

  libremidi::observer_configuration configuration;
  std::shared_ptr<observer_sink> sink;
};
}
}
